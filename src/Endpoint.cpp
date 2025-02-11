/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 * Copyright (C) 2020 - present Proyectos y Sistemas de Mantenimiento SL (eProsima).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "Endpoint.hpp"

#include <cstdlib>

#include <is/json-xtypes/conversion.hpp>

namespace eprosima
{
    namespace is
    {
        namespace sh
        {
            namespace websocket
            {

                //==============================================================================
                struct CallHandle
                {
                    std::string service_name;
                    std::string request_type;
                    std::string reply_type;
                    std::string id;
                    std::shared_ptr<void> connection_handle;
                };

                //==============================================================================
                inline std::shared_ptr<CallHandle> make_call_handle(
                    std::string service_name,
                    std::string request_type,
                    std::string reply_type,
                    std::string id,
                    std::shared_ptr<void> connection_handle)
                {
                    return std::make_shared<CallHandle>(
                        CallHandle{std::move(service_name),
                                   std::move(request_type),
                                   std::move(reply_type),
                                   std::move(id),
                                   std::move(connection_handle)});
                }

                //==============================================================================
                Endpoint::Endpoint(
                    const std::string &name)
                    : _logger(name), _next_service_call_id(1)
                {
                    // Do nothing
                }

                //==============================================================================
                bool Endpoint::configure(
                    const core::RequiredTypes &types,
                    const YAML::Node &configuration,
                    TypeRegistry & /*type_registry*/)
                {
                    if (const YAML::Node encode_node = configuration[YamlEncodingKey])
                    {
                        const std::string encoding_str = [&]() -> std::string
                        {
                            std::string encoding = encode_node.as<std::string>("");
                            std::transform(encoding.begin(), encoding.end(),
                                           encoding.begin(), ::tolower);
                            return encoding;
                        }();

                        if (encoding_str == YamlEncoding_Json)
                        {
                            _logger << utils::Logger::Level::DEBUG
                                    << "Using JSON encoding" << std::endl;

                            _encoding = make_json_encoding();
                        }
                        else
                        {
                            _logger << utils::Logger::Level::ERROR
                                    << "Unknown encoding type was requested: '"
                                    << _encoding << "'" << std::endl;

                            return false;
                        }
                    }
                    else
                    {
                        _logger << utils::Logger::Level::DEBUG
                                << "Using JSON encoding" << std::endl;

                        _encoding = make_json_encoding();
                    }

                    if (!_encoding)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Reached a line '" << __LINE__ << "' that should "
                                << "be impossible. Please report this bug!" << std::endl;

                        return false;
                    }

                    bool success = false;

                    if (configuration["security"] && configuration["security"].as<std::string>() == "none")
                    {
                        _logger << utils::Logger::Level::INFO
                                << "Security disabled, creating TCP endpoint..." << std::endl;

                        _use_security = false;
                        _tcp_endpoint = std::make_shared<TcpEndpoint>(configure_tcp_endpoint(types, configuration));

                        success = _tcp_endpoint != nullptr;
                    }
                    else
                    {
                        _logger << utils::Logger::Level::INFO
                                << "Security enabled, creating TLS endpoint..." << std::endl;

                        _use_security = true;
                        _tls_endpoint = std::make_shared<TlsEndpoint>(configure_tls_endpoint(types, configuration));

                        success = _tls_endpoint != nullptr;
                    }

                    if (success)
                    {
                        _logger << utils::Logger::Level::INFO << "Configured!" << std::endl;
                    }

                    return success;
                }

                //==============================================================================
                bool Endpoint::subscribe(
                    const std::string &topic_name,
                    const xtypes::DynamicType &message_type,
                    SubscriptionCallback *callback,
                    const YAML::Node &configuration)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Subscribing to topic '" << topic_name
                            << "' with topic type '" << message_type.name() << "'" << std::endl;

                    _encoding->add_type(message_type, message_type.name());

                    _startup_messages.emplace_back(
                        _encoding->encode_subscribe_msg(
                            topic_name, message_type.name(), "", configuration));

                    TopicSubscribeInfo &info = _topic_subscribe_info[topic_name];
                    info.type = message_type.name();
                    info.callback = callback;

                    return true;
                }

                bool Endpoint::is_internal_message(
                    void * /*filter_handle*/)
                {
                    // As WebSocket is connection-oriented, there is no need to filter internal messages, since they
                    // are not "published" to the whole network but redirected to a specific subscription.
                    return false;
                }

                //==============================================================================
                std::shared_ptr<TopicPublisher> Endpoint::advertise(
                    const std::string &topic_name,
                    const xtypes::DynamicType &message_type,
                    const YAML::Node &configuration)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Advertising topic publisher '" << topic_name
                            << "' with topic type '" << message_type.name() << "'" << std::endl;

                    _encoding->add_type(message_type, message_type.name());

                    return make_topic_publisher(
                        topic_name, message_type, "", configuration, *this);
                }

                //==============================================================================
                bool Endpoint::create_client_proxy(
                    const std::string &service_name,
                    const xtypes::DynamicType &request_type,
                    const xtypes::DynamicType &reply_type,
                    RequestCallback *callback,
                    const YAML::Node &configuration)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Creating service client proxy for service '" << service_name
                            << "' with request type '" << request_type.name()
                            << "' and reply type '" << reply_type.name() << "'" << std::endl;

                    ClientProxyInfo &info = _client_proxy_info[service_name];
                    info.req_type = request_type.name();
                    info.reply_type = reply_type.name();
                    info.callback = callback;
                    info.configuration = configuration;

                    _encoding->add_type(request_type, request_type.name());
                    _encoding->add_type(reply_type, reply_type.name());

                    // add to connection msgs so the other side knowns we have these services
                    // also, by executing encode_advertise_service_msg we add the types to the services
                    _startup_messages.emplace_back(
                        _encoding->encode_advertise_service_msg(service_name, request_type.name(), reply_type.name(), "", configuration));

                    return true;
                }

                //==============================================================================
                bool Endpoint::create_client_proxy(
                    const std::string &service_name,
                    const xtypes::DynamicType &service_type,
                    RequestCallback *callback,
                    const YAML::Node &configuration)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Creating service client proxy for service '" << service_name
                            << "' with service type '" << service_type.name() << "'" << std::endl;

                    ClientProxyInfo &info = _client_proxy_info[service_name];
                    info.req_type = service_type.name();
                    info.callback = callback;
                    info.configuration = configuration;

                    _encoding->add_type(service_type, service_type.name());

                    return true;
                }

                //==============================================================================
                std::shared_ptr<ServiceProvider> Endpoint::create_service_proxy(
                    const std::string &service_name,
                    const xtypes::DynamicType &service_type,
                    const YAML::Node &configuration)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Creating service server proxy for service '" << service_name
                            << "' with service type '" << service_type.name() << "'" << std::endl;

                    ServiceProviderInfo &info = _service_provider_info[service_name];
                    info.req_type = service_type.name();
                    info.configuration = configuration;

                    return make_service_provider(service_name, *this);
                }

                //==============================================================================
                std::shared_ptr<ServiceProvider> Endpoint::create_service_proxy(
                    const std::string &service_name,
                    const xtypes::DynamicType &request_type,
                    const xtypes::DynamicType &reply_type,
                    const YAML::Node &configuration)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Creating service server proxy for service '" << service_name
                            << "' with request type '" << request_type.name()
                            << "' and reply type '" << reply_type.name() << "'" << std::endl;

                    ServiceProviderInfo &info = _service_provider_info[service_name];
                    info.req_type = request_type.name();
                    info.reply_type = reply_type.name();
                    info.configuration = configuration;

                    _encoding->add_type(request_type, request_type.name());
                    _encoding->add_type(reply_type, reply_type.name());

                    return make_service_provider(service_name, *this);
                }

                //==============================================================================
                void Endpoint::startup_advertisement(
                    const std::string &topic,
                    const xtypes::DynamicType &message_type,
                    const std::string &id,
                    const YAML::Node &configuration)
                {
                    TopicPublishInfo &info = _topic_publish_info[topic];
                    info.type = message_type.name();

                    _startup_messages.emplace_back(
                        _encoding->encode_advertise_msg(
                            topic, message_type.name(), id, configuration));
                }

                //==============================================================================
                bool Endpoint::publish(
                    const std::string &topic,
                    const xtypes::DynamicData &message)
                {
                    const TopicPublishInfo &info = _topic_publish_info.at(topic);

                    // If no one is listening, then don't bother publishing
                    if (info.listeners.empty())
                    {
                        return true;
                    }

                    for (const auto &v_handle : info.listeners)
                    {
                        std::string payload;
                        ErrorCode ec;

                        if (_use_security)
                        {
                            auto connection_handle = _tls_endpoint->get_con_from_hdl(v_handle.first);

                            payload = _encoding->encode_publication_msg(topic, info.type, "", message);
                            if (!payload.empty())
                            {
                                ec = connection_handle->send(payload);
                            }
                        }
                        else
                        {
                            auto connection_handle = _tcp_endpoint->get_con_from_hdl(v_handle.first);

                            payload = _encoding->encode_publication_msg(topic, info.type, "", message);
                            if (!payload.empty())
                            {
                                ec = connection_handle->send(payload);
                            }
                        }

                        if (ec)
                        {
                            _logger << utils::Logger::Level::ERROR
                                    << "Failed to send publication on topic '" << topic
                                    << "', error: " << ec.message() << std::endl;
                        }
                        else
                        {
                            _logger << utils::Logger::Level::INFO
                                    << "Sent publication on topic '" << topic << "': [[ "
                                    << payload << " ]]" << std::endl;
                        }
                    }

                    return true;
                }

                //==============================================================================
                void Endpoint::call_service(
                    const std::string &service,
                    const xtypes::DynamicData &request,
                    ServiceClient &client,
                    std::shared_ptr<void> call_handle)
                {
                    std::size_t id = 0;
                    std::string id_str;
                    {
                        const std::lock_guard<std::mutex> lock(_next_service_call_id_mutex);
                        id = _next_service_call_id++;

                        id_str = std::to_string(id);
                        _service_request_info[id_str] = {&client, std::move(call_handle)};
                    }

                    ServiceProviderInfo &provider_info = _service_provider_info.at(service);

                    const std::string payload = _encoding->encode_call_service_msg(
                        service, provider_info.req_type, request,
                        id_str, provider_info.configuration);

                    if (payload.empty())
                    {
                        return;
                    }

                    ErrorCode ec;

                    if (_use_security)
                    {
                        ec = _tls_endpoint->get_con_from_hdl(provider_info.connection_handle)->send(payload);
                    }
                    else
                    {
                        ec = _tcp_endpoint->get_con_from_hdl(provider_info.connection_handle)->send(payload);
                    }

                    if (ec)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to call service '" << service << "' with request type '"
                                << request.type().name() << "', error: " << ec.message() << std::endl;
                    }
                    else
                    {
                        _logger << utils::Logger::Level::DEBUG
                                << "Service request " << id << ":: Called service '" << service << "' with request type '"
                                << request.type().name() << "', data: [[ " << payload << " ]]" << std::endl;
                    }
                }

                //==============================================================================
                void Endpoint::receive_response(
                    std::shared_ptr<void> v_call_handle,
                    const xtypes::DynamicData &response)
                {
                    const auto &call_handle =
                        *static_cast<const CallHandle *>(v_call_handle.get());

                    std::string payload;
                    ErrorCode ec;

                    if (_use_security)
                    {
                        auto connection_handle = _tls_endpoint->get_con_from_hdl(
                            call_handle.connection_handle);

                        payload = _encoding->encode_service_response_msg(
                            call_handle.service_name,
                            call_handle.reply_type,
                            call_handle.id,
                            response, true);

                        if (!payload.empty())
                        {
                            ec = connection_handle->send(payload);
                        }
                    }
                    else
                    {
                        auto connection_handle = _tcp_endpoint->get_con_from_hdl(
                            call_handle.connection_handle);

                        payload = _encoding->encode_service_response_msg(
                            call_handle.service_name,
                            call_handle.reply_type,
                            call_handle.id,
                            response, true);

                        if (!payload.empty())
                        {
                            ec = connection_handle->send(payload);
                        }
                    }

                    if (ec)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to receive response from service, sent payload: [[ "
                                << payload << " ]], error: " << ec.message() << std::endl;
                    }
                    else
                    {
                        _logger << utils::Logger::Level::DEBUG
                                << "Received response from service: [[ " << payload << " ]]" << std::endl;
                    }
                }

                //==============================================================================
                void Endpoint::receive_topic_advertisement_ws(
                    const std::string &topic_name,
                    const xtypes::DynamicType &message_type,
                    const std::string & /*id*/,
                    std::shared_ptr<void> connection_handle)
                {
                    auto it = _topic_subscribe_info.find(topic_name);
                    if (it != _topic_subscribe_info.end())
                    {
                        TopicSubscribeInfo &info = it->second;
                        if (message_type.name() != info.type)
                        {
                            info.blacklist.insert(connection_handle);

                            _logger << utils::Logger::Level::WARN
                                    << "A remote connection advertised the topic '" << topic_name
                                    << "', to which we want to subscribe to, but with "
                                    << "the wrong message type (" << message_type.name()
                                    << "). The expected type is '" << info.type << "'. Messages from "
                                    << "this connection will be ignored." << std::endl;
                        }
                        else
                        {
                            _logger << utils::Logger::Level::INFO
                                    << "Advertising topic '" << topic_name
                                    << "' with message type '" << message_type.name() << "'" << std::endl;

                            info.blacklist.erase(connection_handle);
                        }
                    }
                    else
                    {
                        _logger << utils::Logger::Level::WARN
                                << "A remote connection advertised the topic '" << topic_name
                                << "' but no subscriber was found for this topic. Maybe you mispelled the topic name?"
                                << std::endl;
                    }
                }

                //==============================================================================
                void Endpoint::receive_topic_unadvertisement_ws(
                    const std::string & /*topic_name*/,
                    const std::string & /*id*/,
                    std::shared_ptr<void> /*connection_handle*/)
                {
                }

                //==============================================================================
                void Endpoint::receive_publication_ws(
                    const std::string &topic_name,
                    const xtypes::DynamicData &message,
                    std::shared_ptr<void> connection_handle)
                {
                    try
                    {
                        /* _logger << utils::Logger::Level::DEBUG
                                 << "Received message on subscriber '" << topic_name
                                 << "', data: [[ " << json_xtypes::convert(message) << " ]]" << std::endl;*/

                        auto it = _topic_subscribe_info.find(topic_name);
                        if (it == _topic_subscribe_info.end())
                        {
                            return;
                        }

                        TopicSubscribeInfo &info = it->second;
                        if (info.blacklist.count(connection_handle) > 0)
                        {
                            return;
                        }

                        (*info.callback)(message, nullptr);
                    }
                    catch (const json_xtypes::UnsupportedType &unsupported)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to receive publication for topic '" << topic_name
                                << "' with type '" << message.type().name() << "', reason: [[ "
                                << unsupported.what() << " ]]" << std::endl;
                    }
                    catch (const json_xtypes::Json::exception &exception)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to receive publication for topic '" << topic_name
                                << "' with type '" << message.type().name() << "' because conversion from xTypes"
                                << " to JSON failed. Details: [[ "
                                << exception.what() << " ]]" << std::endl;
                    }
                }

                //==============================================================================
                void Endpoint::receive_subscribe_request_ws(
                    const std::string &topic_name,
                    const xtypes::DynamicType *message_type,
                    const std::string &id,
                    std::shared_ptr<void> connection_handle)
                {
                    auto insertion = _topic_publish_info.insert(
                        std::make_pair(topic_name, TopicPublishInfo{}));
                    const bool inserted = insertion.second;
                    TopicPublishInfo &info = insertion.first->second;

                    if (inserted)
                    {
                        _logger << utils::Logger::Level::WARN
                                << "Received subscription request for the topic '" << topic_name
                                << "', that we are not currently advertising" << std::endl;
                    }
                    else
                    {
                        if (message_type != nullptr && message_type->name() != info.type)
                        {
                            _logger << utils::Logger::Level::ERROR
                                    << "Received subscription request for topic '" << topic_name
                                    << "', but the requested message type '" << message_type->name()
                                    << "' does not match the one we are publishing "
                                    << "(" << info.type << ")" << std::endl;
                            return;
                        }

                        _logger << utils::Logger::Level::DEBUG
                                << "Received subscription request for topic '" << topic_name
                                << "', with message type '" << message_type->name() << "'" << std::endl;
                    }

                    info.listeners[connection_handle].insert(id);
                }

                //==============================================================================
                void Endpoint::receive_unsubscribe_request_ws(
                    const std::string &topic_name,
                    const std::string &id,
                    std::shared_ptr<void> connection_handle)
                {
                    auto it = _topic_publish_info.find(topic_name);
                    if (it == _topic_publish_info.end())
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Received an unsubscription request for the topic '" << topic_name
                                << "', which we are not currently advertising" << std::endl;
                        return;
                    }

                    TopicPublishInfo &info = it->second;
                    auto lit = info.listeners.find(connection_handle);

                    if (lit == info.listeners.end())
                    {
                        return;
                    }

                    _logger << utils::Logger::Level::DEBUG
                            << "Received unsubscription request for topic '" << topic_name << "'" << std::endl;

                    if (id.empty())
                    {
                        // If id is empty, then we should erase this connection as a listener
                        // entirely.
                        info.listeners.erase(lit);
                        return;
                    }

                    std::unordered_set<std::string> &listeners = lit->second;
                    listeners.erase(id);

                    if (listeners.empty())
                    {
                        // If no more unique ids are listening from this connection, then
                        // erase it entirely.
                        info.listeners.erase(lit);
                    }
                }

                //==============================================================================
                void Endpoint::receive_service_request_ws(
                    const std::string &service_name,
                    const xtypes::DynamicData &request,
                    const std::string &id,
                    std::shared_ptr<void> connection_handle)
                {
                    try
                    {
                        auto it = _client_proxy_info.find(service_name);
                        if (it == _client_proxy_info.end())
                        {
                            _logger << utils::Logger::Level::ERROR
                                    << "Received a service request for a service '"
                                    << service_name << "' that we are not providing!" << std::endl;

                            return;
                        }
                        else
                        {
                            _logger << utils::Logger::Level::DEBUG
                                    << "Received a service request for service '" << service_name
                                    << "', data: [[ " << json_xtypes::convert(request) << " ]]" << std::endl;
                        }

                        ClientProxyInfo &info = it->second;
                        (*info.callback)(request, *this,
                                         make_call_handle(service_name, info.req_type, info.reply_type,
                                                          id, connection_handle));
                    }
                    catch (const json_xtypes::UnsupportedType &unsupported)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to receive request for service '" << service_name
                                << "' with request type '" << request.type().name() << "', reason: [[ "
                                << unsupported.what() << " ]]" << std::endl;
                    }
                    catch (const json_xtypes::Json::exception &exception)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to receive request for service '" << service_name
                                << "' with request type '" << request.type().name() << "' because conversion"
                                << " from xTypes to JSON failed. Details: [[ "
                                << exception.what() << " ]]" << std::endl;
                    }
                }

                //==============================================================================
                void Endpoint::receive_service_advertisement_ws(
                    const std::string &service_name,
                    const xtypes::DynamicType &req_type,
                    const xtypes::DynamicType &reply_type,
                    std::shared_ptr<void> connection_handle)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Received advertise for service '" << service_name
                            << "' with request type '" << req_type.name() << "', and reply type '"
                            << reply_type.name() << "'" << std::endl;

                    _service_provider_info[service_name] =
                        ServiceProviderInfo{req_type.name(), reply_type.name(), connection_handle, YAML::Node{}};
                }

                //==============================================================================
                void Endpoint::receive_service_unadvertisement_ws(
                    const std::string &service_name,
                    const xtypes::DynamicType * /*service_type*/,
                    std::shared_ptr<void> connection_handle)
                {
                    auto it = _service_provider_info.find(service_name);
                    if (it == _service_provider_info.end())
                    {
                        _logger << utils::Logger::Level::WARN
                                << "Received unadvertise for the service '" << service_name
                                << "', that we are not currently advertising" << std::endl;
                        return;
                    }

                    _logger << utils::Logger::Level::DEBUG
                            << "Received unadvertise for service '" << service_name << "'" << std::endl;

                    if (it->second.connection_handle == connection_handle)
                    {
                        _service_provider_info.erase(it);
                    }
                }

                //==============================================================================
                void Endpoint::receive_service_response_ws(
                    const std::string &service_name,
                    const xtypes::DynamicData &response,
                    const std::string &id,
                    std::shared_ptr<void> /*connection_handle*/)
                {
                    try
                    {
                        auto it = _service_request_info.find(id);
                        if (it == _service_request_info.end())
                        {
                            _logger << utils::Logger::Level::ERROR
                                    << "A remote connection provided a service response for service '"
                                    << service_name << "' with an unrecognized id '" << id << "'" << std::endl;

                            return;
                        }

                        // TODO(MXG): We could use the service_name and connection_handle info to
                        // verify that the service response is coming from the source that we were
                        // expecting.
                        ServiceRequestInfo &info = it->second;

                        _logger << utils::Logger::Level::DEBUG
                                << "Service response " << id << ":: Receive response for service '" << service_name << "', data: [[ "
                                << json_xtypes::convert(response) << " ]]" << std::endl;

                        info.client->receive_response(info.call_handle, response);

                        _service_request_info.erase(it);
                    }
                    catch (const json_xtypes::UnsupportedType &unsupported)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to receive response from service '" << service_name
                                << "' with reply type '" << response.type().name() << "', reason: [[ "
                                << unsupported.what() << " ]]" << std::endl;
                    }
                    catch (const json_xtypes::Json::exception &exception)
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "Failed to receive request for service '" << service_name
                                << "' with reply type '" << response.type().name() << "' because conversion"
                                << " from xTypes to JSON failed. Details: [[ "
                                << exception.what() << " ]]" << std::endl;
                    }
                }

                //==============================================================================
                const Encoding &Endpoint::get_encoding() const
                {
                    return *_encoding;
                }

                //==============================================================================
                void Endpoint::notify_connection_opened(
                    const TlsConnectionPtr &connection_handle)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "TLS connection " << connection_handle << " opened" << std::endl;

                    for (const std::string &msg : _startup_messages)
                    {
                        connection_handle->send(msg);
                    }
                }

                void Endpoint::notify_connection_opened(
                    const TcpConnectionPtr &connection_handle)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "TCP connection " << connection_handle << " opened" << std::endl;

                    for (const std::string &msg : _startup_messages)
                    {
                        connection_handle->send(msg);
                    }
                }

                //==============================================================================
                void Endpoint::notify_connection_closed(
                    const std::shared_ptr<void> &connection_handle)
                {
                    _logger << utils::Logger::Level::DEBUG
                            << "Connection " << connection_handle << " closed" << std::endl;

                    for (auto &entry : _topic_subscribe_info)
                    {
                        entry.second.blacklist.erase(connection_handle);
                    }

                    for (auto &entry : _topic_publish_info)
                    {
                        entry.second.listeners.erase(connection_handle);
                    }

                    std::vector<std::string> lost_services;
                    lost_services.reserve(_service_provider_info.size());
                    for (auto &entry : _service_provider_info)
                    {
                        if (entry.second.connection_handle == connection_handle)
                        {
                            lost_services.push_back(entry.first);
                        }
                    }

                    for (const std::string &s : lost_services)
                    {
                        _service_provider_info.erase(s);
                    }

                    // NOTE(MXG): We'll leave _service_request_info alone, because it's feasible
                    // that the service response might arrive later after the other side has
                    // reconnected. The downside is this could allow lost services to accumulate.
                }

                //==============================================================================
                int32_t Endpoint::parse_port(
                    const YAML::Node &configuration)
                {
                    if (const YAML::Node port_node = configuration[YamlPortKey])
                    {
                        try
                        {
                            auto port = port_node.as<int>();

                            _logger << utils::Logger::Level::DEBUG
                                    << "Using port: " << port << std::endl;

                            return port_node.as<int>();
                        }
                        catch (const YAML::InvalidNode &v)
                        {
                            _logger << utils::Logger::Level::ERROR
                                    << "Could not parse an unsigned integer value for the port setting '"
                                    << port_node << "': " << v.what() << std::endl;
                        }
                    }
                    else
                    {
                        _logger << utils::Logger::Level::ERROR
                                << "You must specify a port setting in your WebSocket configuration!"
                                << std::endl;
                    }

                    return -1;
                }

            } //  namespace websocket
        }     //  namespace sh
    }         //  namespace is
} //  namespace eprosima
