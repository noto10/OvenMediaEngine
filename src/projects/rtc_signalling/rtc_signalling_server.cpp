//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include "rtc_signalling_server.h"
#include "rtc_ice_candidate.h"
#include "rtc_signalling_server_private.h"

#include <utility>

#include <ice/ice.h>

#define USE_P2P                             0

RtcSignallingServer::RtcSignallingServer(const info::Application &application_info, std::shared_ptr<MediaRouteApplicationInterface> application)
	: _application_info(application_info),
	  _application(std::move(application))
{
}

bool RtcSignallingServer::Start(const ov::SocketAddress &address, const std::shared_ptr<Certificate> &certificate, const std::shared_ptr<Certificate> &chain_certificate)
{
	if(_http_server != nullptr)
	{
		OV_ASSERT(false, "Server is already running");
		return false;
	}

	if(certificate != nullptr)
	{
		auto https_server = std::make_shared<HttpsServer>();

		https_server->SetLocalCertificate(certificate);
		https_server->SetChainCertificate(chain_certificate);

		_http_server = https_server;
	}
	else
	{
		_http_server = std::make_shared<HttpServer>();
	}

	return InitializeWebSocketServer() && _http_server->Start(address);
}

bool RtcSignallingServer::InitializeWebSocketServer()
{
	auto web_socket = std::make_shared<WebSocketInterceptor>();

	web_socket->SetConnectionHandler(
		[this](const std::shared_ptr<WebSocketClient> &response) -> bool
		{
			auto remote = response->GetResponse()->GetRemote();

			if(remote == nullptr)
			{
				OV_ASSERT(false, "Cannot find the client information: %s", response->ToString().CStr());
				return false;
			}

			ov::String description = remote->ToString();

			logti("New client is connected: %s", description.CStr());

			auto tokens = response->GetRequest()->GetUri().Split("/");

			// "/<app>/<stream>"
			if(tokens.size() < 3)
			{
				logti("Invalid request from %s. Disconnecting...", description.CStr());
				return false;
			}

			auto info = std::make_shared<RtcSignallingInfo>(
				// application_name
				tokens[1],
				// stream_name
				tokens[2],

				// id
				P2P_INVALID_PEER_ID,
				// peer_info,
				nullptr,
				// offer_sdp
				nullptr,
				// peer_sdp
				nullptr,
				// local_candidates
				std::vector<RtcIceCandidate>(),
				// remote_candidates
				std::vector<RtcIceCandidate>()
			);

			{
				std::lock_guard<std::mutex> lock_guard(_client_list_mutex);

				while(true)
				{
					peer_id_t id = ov::Random::GenerateInt32(1, INT32_MAX);

					auto client = _client_list.find(id);

					if(client == _client_list.end())
					{
						info->id = id;
						_client_list[id] = info;

						break;
					}
				}
			}

			response->GetRequest()->SetExtra(info);

			return true;
		});

	web_socket->SetMessageHandler(
		[this](const std::shared_ptr<WebSocketClient> &response, const std::shared_ptr<const WebSocketFrame> &message) -> bool
		{
			logtp("The client sent a message:\n%s", message->GetPayload()->Dump().CStr());

			auto info = response->GetRequest()->GetExtraAs<RtcSignallingInfo>();

			if(info == nullptr)
			{
				// If you enter here, there is only the following situation:
				//
				// 1. An error occurred during the connection (request was wrong)
				// 2. After the connection is lost, the callback is called late
				logtw("Could not find client information: %s", response->ToString().CStr());

				return false;
			}

			ov::JsonObject object = ov::Json::Parse(message->GetPayload());

			if(object.IsNull())
			{
				logtw("Invalid request message from %s", response->ToString().CStr());
				return false;
			}

			// TODO(dimiden): 이렇게 호출하면 "command": null 이 추가되어버림. 개선 필요
			Json::Value &command_value = object.GetJsonValue()["command"];

			if(command_value.isNull())
			{
				logtw("Invalid request message from %s", response->ToString().CStr());
				return false;
			}

			ov::String command = ov::Converter::ToString(command_value);

			logtd("Trying to dispatch command: %s...", command.CStr());

			auto error = DispatchCommand(command, object, info, response, message);

			if(error != nullptr)
			{
				logte("An error occurred while dispatch command: %s, disconnecting...", command.CStr());

				ov::JsonObject response_json;
				Json::Value &value = response_json.GetJsonValue();

				value["code"] = error->GetCode();
				value["error"] = error->GetMessage().CStr();

				response->Send(response_json.ToString());

				return false;
			}

			return true;
		});

	web_socket->SetErrorHandler(
		[this](const std::shared_ptr<WebSocketClient> &response, const std::shared_ptr<const ov::Error> &error) -> void
		{
			logtw("An error occurred: %s", error->ToString().CStr());
		});

	web_socket->SetCloseHandler(
		[this](const std::shared_ptr<WebSocketClient> &response) -> void
		{
			auto info = response->GetRequest()->GetExtraAs<RtcSignallingInfo>();

			if(info != nullptr)
			{
				if(info->id != P2P_INVALID_PEER_ID)
				{
					// The client is disconnected without send "close" command

					// Forces the session to be cleaned up by sending a stop command
					DispatchStop(info);
				}

				logti("Client is disconnected: %s (%s / %s, ufrag: local: %s, remote: %s)",
				      response->ToString().CStr(),
				      info->application_name.CStr(), info->stream_name.CStr(),
				      (info->offer_sdp != nullptr) ? info->offer_sdp->GetIceUfrag().CStr() : "(N/A)",
				      (info->peer_sdp != nullptr) ? info->peer_sdp->GetIceUfrag().CStr() : "(N/A)"
				);
			}
		});

	return _http_server->AddInterceptor(web_socket);
}

bool RtcSignallingServer::AddObserver(const std::shared_ptr<RtcSignallingObserver> &observer)
{
	// 기존에 등록된 observer가 있는지 확인
	for(const auto &item : _observers)
	{
		if(item == observer)
		{
			// 기존에 등록되어 있음
			logtw("%p is already observer of RtcSignallingServer", observer.get());
			return false;
		}
	}

	_observers.push_back(observer);

	return true;
}

bool RtcSignallingServer::RemoveObserver(const std::shared_ptr<RtcSignallingObserver> &observer)
{
	auto item = std::find_if(_observers.begin(), _observers.end(), [&](std::shared_ptr<RtcSignallingObserver> const &value) -> bool
	{
		return value == observer;
	});

	if(item == _observers.end())
	{
		// 기존에 등록되어 있지 않음
		logtw("%p is not registered observer", observer.get());
		return false;
	}

	_observers.erase(item);

	return true;
}

bool RtcSignallingServer::Disconnect(const ov::String &application_name, const ov::String &stream_name, const std::shared_ptr<SessionDescription> &peer_sdp)
{
	bool disconnected = _http_server->Disconnect(
		[application_name, stream_name, peer_sdp](const std::shared_ptr<HttpClient> &client) -> bool
		{
			auto info = client->GetRequest()->GetExtraAs<RtcSignallingInfo>();

			if(info == nullptr)
			{
				// Client disconnected while Connect () is being processed
			}
			else
			{
				return
					(info->application_name == application_name) &&
					(info->stream_name == stream_name) &&
					((info->peer_sdp != nullptr) && (*(info->peer_sdp) == *peer_sdp));
			}

			return false;
		});

	if(disconnected == false)
	{
		// ICE 연결이 끊어져 Disconnect()이 호출 된 직후, _http_server->Disconnect()이 실행되기 전 타이밍에
		// WebSocket 연결이 끊어져서 HttpServer::OnDisconnected() 이 처리되고 나면 실패 할 수 있음
	}

	return disconnected;
}

bool RtcSignallingServer::Stop()
{
	if(_http_server == nullptr)
	{
		return false;
	}

	if(_http_server->Stop())
	{
		_http_server = nullptr;

		return true;
	}

	return false;
}

std::shared_ptr<ov::Error> RtcSignallingServer::DispatchCommand(const ov::String &command, const ov::JsonObject &object, std::shared_ptr<RtcSignallingInfo> &info, const std::shared_ptr<WebSocketClient> &response, const std::shared_ptr<const WebSocketFrame> &message)
{
	if(command == "request_offer")
	{
		return DispatchRequestOffer(info, response);
	}

	if(info->id != object.GetInt64Value("id"))
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid ID");
	}
	else if(command == "answer")
	{
		return DispatchAnswer(object, info);
	}
	else if(command == "candidate")
	{
		return DispatchCandidate(object, info);
	}
	else if(command == "offer_p2p")
	{
		return DispatchOfferP2P(object, info);
	}
	else if(command == "candidate_p2p")
	{
		return DispatchCandidateP2P(object, info);
	}
	else if(command == "stop")
	{
		return DispatchStop(info);
	}

	// Unknown command
	return ov::Error::CreateError(HttpStatusCode::BadRequest, "Unknown command: %s", command.CStr());
}

std::shared_ptr<ov::Error> RtcSignallingServer::DispatchRequestOffer(std::shared_ptr<RtcSignallingInfo> &info, const std::shared_ptr<WebSocketClient> &response)
{
	ov::String application_name = info->application_name;
	ov::String stream_name = info->stream_name;

	std::shared_ptr<SessionDescription> sdp = nullptr;
	std::shared_ptr<ov::Error> error = nullptr;

	logtd("Trying to find p2p host for client %s...", response->ToString().CStr());

	// Check if there is a host that can accept this client
	auto peer_info = _p2p_manager.CreatePeerInfo(info->id, response);

	if(peer_info == nullptr)
	{
		return ov::Error::CreateError(HttpStatusCode::InternalServerError, "Cannot parse peer info from user agent: %s", response->GetRequest()->GetHeader("USER-AGENT").CStr());
	}

	info->peer_info = peer_info;

	std::shared_ptr<RtcPeerInfo> host_peer;

	{
		std::lock_guard<std::mutex> lock_guard(_client_list_mutex);

		if(info->peer_info == nullptr)
		{
			// If client is stopped or disconnected and DispatchStop is executed, it enters here
		}
		else
		{
#if USE_P2P
			host_peer = _p2p_manager.TryToRegisterAsClientPeer(peer_info);
#endif // USE_P2P
		}
	}

	if(host_peer == nullptr)
	{
		logtd("There is no p2p host for client %s.", response->ToString().CStr());

		// None of the hosts can accept this client
		std::find_if(_observers.begin(), _observers.end(), [info, &sdp, application_name, stream_name](auto &observer) -> bool
		{
			// Ask observer to fill local_candidates
			sdp = observer->OnRequestOffer(application_name, stream_name, &(info->local_candidates));
			return sdp != nullptr;
		});

		if(sdp != nullptr)
		{
			logtd("SDP is generated successfully");

			if(_p2p_manager.RegisterAsHostPeer(peer_info) == false)
			{
				OV_ASSERT2(false);
				return ov::Error::CreateError(HttpStatusCode::InternalServerError, "Could not add host peer");
			}

			ov::JsonObject response_json;

			Json::Value &value = response_json.GetJsonValue();

			// SDP를 계산함
			Json::Value &sdp_value = value["sdp"];

			ov::String offer_sdp;

			if(sdp->ToString(offer_sdp))
			{
				value["command"] = "offer";
				value["id"] = info->id;
				value["peer_id"] = P2P_OME_PEER_ID;
				sdp_value["sdp"] = offer_sdp.CStr();
				sdp_value["type"] = "offer";

				// candidates: [ <candidate>, <candidate>, ... ]
				Json::Value candidates(Json::ValueType::arrayValue);

				// candiate:
				// {
				//     "candidate":"candidate:0 1 UDP 50 192.168.0.183 10000 typ host generation 0",
				//     "sdpMLineIndex":0,
				//     "sdpMid":"video"
				// }
				// local candidate 목록을 client로 보냄
				for(const auto &candidate : info->local_candidates)
				{
					Json::Value item;

					item["candidate"] = candidate.GetCandidateString().CStr();
					item["sdpMLineIndex"] = candidate.GetSdpMLineIndex();
					if(candidate.GetSdpMid().IsEmpty() == false)
					{
						item["sdpMid"] = candidate.GetSdpMid().CStr();
					}

					candidates.append(item);
				}
				value["candidates"] = candidates;
				value["code"] = static_cast<int>(HttpStatusCode::OK);

				info->offer_sdp = sdp;

				response->Send(response_json.ToString());
			}
			else
			{
				logtw("Could not create SDP for stream %s", info->stream_name.CStr());
				error = ov::Error::CreateError(HttpStatusCode::NotFound, "Cannot create offer");
			}
		}
		else
		{
			// cannot create offer
			error = ov::Error::CreateError(HttpStatusCode::NotFound, "Cannot create offer");
		}
	}
	else
	{
		// Found a host that can accept this client
		logtd("P2P host found: %s", host_peer->ToString().CStr());

		// Send 'request_offer_p2p' command to the host
		Json::Value value;

		value["command"] = "request_offer_p2p";
		value["id"] = host_peer->GetId();
		value["peer_id"] = peer_info->GetId();

		host_peer->GetResponse()->Send(value);

		// Wait for 'offer_p2p' command from the host

		// TODO(dimiden): Timeout is required because host peer may not give offer for too long
	}

	return error;
}

std::shared_ptr<ov::Error> RtcSignallingServer::DispatchAnswer(const ov::JsonObject &object, std::shared_ptr<RtcSignallingInfo> &info)
{
	auto &peer_info = info->peer_info;

	if(peer_info == nullptr)
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Could not find peer id: %d", info->id);
	}

	const Json::Value &sdp_value = object.GetJsonValue("sdp");

	// Validate SDP
	if(sdp_value.isNull() || (sdp_value.isObject() == false))
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "There is no SDP");
	}

	const Json::Value &sdp_type = sdp_value["type"];

	if((sdp_type.isString() == false) || (sdp_type != "answer"))
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid SDP type");
	}

	if(sdp_value["sdp"].isString() == false)
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "SDP must be a string");
	}

	if(peer_info->IsHost())
	{
		logtd("The host peer sent a answer: %s", object.ToString().CStr());

		info->peer_sdp = std::make_shared<SessionDescription>();

		if(info->peer_sdp->FromString(sdp_value["sdp"].asCString()))
		{
			for(auto &observer : _observers)
			{
				logtd("Trying to callback OnAddRemoteDescription to %p (%s / %s)...", observer.get(), info->application_name.CStr(), info->stream_name.CStr());

				observer->OnAddRemoteDescription(info->application_name, info->stream_name, info->offer_sdp, info->peer_sdp);
			}
		}
		else
		{
			// SDP를 파싱할 수 없음
			return ov::Error::CreateError(HttpStatusCode::BadRequest, "Could not parse SDP");
		}
	}
	else
	{
		logtd("The client peer sent a answer: %s", object.ToString().CStr());

		// The client peer sent this answer
		auto peer_id = object.GetIntValue("peer_id");
		auto host_peer = peer_info->GetHostPeer();

		if(host_peer == nullptr)
		{
			OV_ASSERT2(false);
			return ov::Error::CreateError(HttpStatusCode::InternalServerError, "Could not find host information");
		}

		if(host_peer->GetId() != peer_id)
		{
			return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid peer id: %d", peer_id);
		}

		Json::Value value;

		value["command"] = "answer";
		value["id"] = host_peer->GetId();
		value["peer_id"] = peer_info->GetId();
		value["sdp"] = sdp_value;

		host_peer->GetResponse()->Send(value);
	}

	return nullptr;
}

std::shared_ptr<ov::Error> RtcSignallingServer::DispatchCandidate(const ov::JsonObject &object, std::shared_ptr<RtcSignallingInfo> &info)
{
	const Json::Value &candidates_value = object.GetJsonValue("candidates");

	if(candidates_value.isNull())
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "There is no candidate list");
	}

	if(candidates_value.isArray() == false)
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Candidates must be array");
	}

	auto peer_id = object.GetIntValue("peer_id");
	auto peer_info = _p2p_manager.FindPeer(peer_id);

	if(peer_info == nullptr)
	{
		// Host -> OME
		for(const auto &candidate_iterator : candidates_value)
		{
			ov::String candidate = ov::Converter::ToString(candidate_iterator["candidate"]);
			uint32_t sdp_m_line_index = ov::Converter::ToUInt32(candidate_iterator["sdpMLineIndex"]);
			ov::String sdp_mid = ov::Converter::ToString(candidate_iterator["sdpMid"]);
			ov::String username_fragment = ov::Converter::ToString(candidate_iterator["usernameFragment"]);

			auto ice_candidate = std::make_shared<RtcIceCandidate>(sdp_m_line_index, sdp_mid);

			if(ice_candidate->ParseFromString(candidate) == false)
			{
				return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid candidate: %s", candidate.CStr());
			}

			for(auto &observer : _observers)
			{
				observer->OnIceCandidate(info->application_name, info->stream_name, ice_candidate, username_fragment);
			}
		}
	}
	else
	{
		// Client -> (OME) -> Host
		Json::Value value;

		value["command"] = "candidate_p2p";
		value["id"] = info->id;
		value["peer_id"] = peer_info->GetId();
		value["candidates"] = candidates_value;

		peer_info->GetResponse()->Send(value);
	}

	return nullptr;
}

std::shared_ptr<ov::Error> RtcSignallingServer::DispatchOfferP2P(const ov::JsonObject &object, std::shared_ptr<RtcSignallingInfo> &info)
{
	auto &host = info->peer_info;

	if(host == nullptr)
	{
		return ov::Error::CreateError(HttpStatusCode::InternalServerError, "Peer %d is not exists", info->id);
	}

	auto peer_id = object.GetIntValue("peer_id");
	auto client_peer = _p2p_manager.GetClientPeerOf(host, peer_id);

	if(client_peer == nullptr)
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid peer_id: %d", peer_id);
	}

	auto sdp_value = object.GetJsonValue("sdp");
	auto candidates = object.GetJsonValue("candidates");

	// Validate SDP
	if(sdp_value.isNull() || (sdp_value.isObject() == false))
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid SDP: %s", sdp_value.asCString());
	}

	const Json::Value &sdp_type = sdp_value["type"];

	if((sdp_type.isString() == false) || (sdp_type != "offer"))
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid SDP type");
	}

	if(sdp_value["sdp"].isString() == false)
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "SDP must be a string");
	}

	if((candidates.isNull() == false) && (candidates.isArray() == false))
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Candidates must be array, but: %d", candidates.type());
	}

	Json::Value value;

	value["command"] = "offer";
	value["id"] = client_peer->GetId();
	value["peer_id"] = host->GetId();
	value["sdp"] = sdp_value;
	if(candidates.isNull() == false)
	{
		value["candidates"] = candidates;
	}

	client_peer->GetResponse()->Send(value);

	return nullptr;
}

std::shared_ptr<ov::Error> RtcSignallingServer::DispatchCandidateP2P(const ov::JsonObject &object, std::shared_ptr<RtcSignallingInfo> &info)
{
	auto &host = info->peer_info;

	if(host == nullptr)
	{
		return ov::Error::CreateError(HttpStatusCode::InternalServerError, "Peer %d is not exists", info->id);
	}

	auto peer_id = object.GetIntValue("peer_id");
	auto client_peer = _p2p_manager.GetClientPeerOf(host, peer_id);

	if(client_peer == nullptr)
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Invalid peer_id: %d", peer_id);
	}

	const Json::Value &candidates_value = object.GetJsonValue("candidates");

	if(candidates_value.isNull())
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "There is no candidate list");
	}

	if(candidates_value.isArray() == false)
	{
		return ov::Error::CreateError(HttpStatusCode::BadRequest, "Candidates must be array");
	}

	Json::Value candidates;

	candidates["command"] = "candidate";
	candidates["id"] = client_peer->GetId();
	candidates["peer_id"] = info->id;
	candidates["candidates"] = candidates_value;

	client_peer->GetResponse()->Send(candidates);

	return nullptr;
}

std::shared_ptr<ov::Error> RtcSignallingServer::DispatchStop(std::shared_ptr<RtcSignallingInfo> &info)
{
	bool result = true;

	if(info->peer_sdp != nullptr)
	{
		for(auto &observer : _observers)
		{
			logtd("Trying to callback OnStopCommand to %p for client %d (%s / %s)...", observer.get(), info->id, info->application_name.CStr(), info->stream_name.CStr());

			if(observer->OnStopCommand(info->application_name, info->stream_name, info->offer_sdp, info->peer_sdp) == false)
			{
				result = false;
			}
		}
	}

	{
		std::shared_ptr<RtcPeerInfo> peer_info;

		{
			std::lock_guard<std::mutex> lock_guard(_client_list_mutex);

			peer_info = info->peer_info;

			info->peer_info = nullptr;

			if(info->id != P2P_INVALID_PEER_ID)
			{
				_client_list.erase(info->id);
				info->id = P2P_INVALID_PEER_ID;
			}
		}

		if(peer_info != nullptr)
		{
			logtd("Deleting peer %s from p2p manager...", peer_info->ToString().CStr());

			_p2p_manager.RemovePeer(peer_info);

			if(peer_info->IsHost())
			{
				// Host peer -> OME

				// Broadcast to client peers
				auto client_list = _p2p_manager.GetClientPeerList(peer_info);

				for(auto &client : client_list)
				{
					auto &client_info = client.second;

					Json::Value value;

					value["command"] = "stop";
					value["id"] = client_info->GetId();
					value["peer_id"] = peer_info->GetId();

					client_info->GetResponse()->Send(value);
				}
			}
			else
			{
				// Client peer -> OME

				// Send to host peer
				auto host_info = peer_info->GetHostPeer();

				if(host_info != nullptr)
				{
					Json::Value value;

					value["command"] = "stop";
					value["id"] = host_info->GetId();
					value["peer_id"] = peer_info->GetId();

					host_info->GetResponse()->Send(value);
				}
				else
				{
					OV_ASSERT2(false);
				}
			}
		}
		else
		{
			// It enters here when if the peer has never requested a request_offer or DispatchRequestOffer() is in progress on another thread
		}
	}

	if(result == false)
	{
		return ov::Error::CreateError(HttpStatusCode::InternalServerError, "Cannot dispatch stop command");
	}

	return nullptr;
}
