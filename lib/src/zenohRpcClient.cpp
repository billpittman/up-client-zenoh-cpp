/*
 * Copyright (c) 2024 General Motors GTO LLC
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 * SPDX-FileType: SOURCE
 * SPDX-FileCopyrightText: 2024 General Motors GTO LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <up-client-zenoh-cpp/rpc/zenohRpcClient.h>
#include <up-client-zenoh-cpp/session/zenohSessionManager.h>
#include <up-cpp/uuid/serializer/UuidSerializer.h>
#include <up-cpp/uri/serializer/LongUriSerializer.h>
#include <up-cpp/transport/datamodel/UPayload.h>
#include <up-cpp/transport/builder/UAttributesBuilder.h>
#include <up-cpp/uuid/factory/Uuidv8Factory.h>
#include <up-cpp/utils/ThreadPool.h>
#include <up-core-api/ustatus.pb.h>
#include <up-core-api/uattributes.pb.h>
#include <spdlog/spdlog.h>
#include <zenoh.h>

using namespace uprotocol::utransport;
using namespace uprotocol::uuid;
using namespace uprotocol::uri;
using namespace uprotocol::utils;

ZenohRpcClient& ZenohRpcClient::instance(void) noexcept {
    
    static ZenohRpcClient rpcClient;

    return rpcClient;
}

UStatus ZenohRpcClient::init() noexcept {

    UStatus status;

    if (0 == refCount_) {

        std::lock_guard<std::mutex> lock(mutex_);

        if (0 == refCount_) {
            /* by default initialized to empty strings */
            ZenohSessionManagerConfig config;

            if (UCode::OK != ZenohSessionManager::instance().init(config)) {
                spdlog::error("zenohSessionManager::instance().init() failed");
                status.set_code(UCode::UNAVAILABLE);
                return status;
            }

            if (ZenohSessionManager::instance().getSession().has_value()) {
                session_ = ZenohSessionManager::instance().getSession().value();
            } else {
                status.set_code(UCode::UNAVAILABLE);
                return status;
            }

            threadPool_ = make_shared<ThreadPool>(queueSize_, maxNumOfCuncurrentRequests_);
            if (nullptr == threadPool_) {
                spdlog::error("failed to create thread pool");
                status.set_code(UCode::UNAVAILABLE);
                return status;
            }
        }
        refCount_.fetch_add(1);

    } else {
        refCount_.fetch_add(1);
    }

    status.set_code(UCode::OK);

    return status;
}

UStatus ZenohRpcClient::term() noexcept {

    UStatus status;
    
    std::lock_guard<std::mutex> lock(mutex_);

    refCount_.fetch_sub(1);

    if (0 == refCount_) {

        if (UCode::OK != ZenohSessionManager::instance().term()) {
            spdlog::error("zenohSessionManager::instance().term() failed");
            status.set_code(UCode::UNAVAILABLE);
            return status;
        }
    }

    status.set_code(UCode::OK);

    return status;
}

std::future<UMessage> ZenohRpcClient::invokeMethod(const UUri &topic, 
                                                   const UPayload &payload, 
                                                   const CallOptions &options) noexcept {
    std::future<UMessage> future;
    z_owned_bytes_map_t map;
    z_owned_reply_channel_t *channel = nullptr;
    z_get_options_t opts = z_get_options_default();
    UCode status = UCode::INTERNAL;

    if (0 == refCount_) {
        spdlog::error("ZenohRpcClient is not initialized");
        return future;
    }

    if (false == isRPCMethod(topic.resource())) {
        spdlog::error("URI is not of RPC type");
        return future;
    }

    if (UPriority::UPRIORITY_CS4 > options.priority()) {
        spdlog::error("Prirority is smaller then UPRIORITY_CS4");
        return future;
    }

    do {

        auto uriHash = std::hash<std::string>{}(LongUriSerializer::serialize(topic));
        auto uuid = Uuidv8Factory::create();
    
        UAttributesBuilder builder(uuid, UMessageType::UMESSAGE_TYPE_REQUEST, UPriority::UPRIORITY_CS0);

        if (options.has_ttl()) {
            builder.setTTL(options.ttl());
            opts.timeout_ms = options.ttl();
        } else {
            opts.timeout_ms = requestTimeoutMs_;
        }

        builder.setPriority(options.priority());

        UAttributes attributes = builder.build();

        // Serialize UAttributes
        size_t attrSize = attributes.ByteSizeLong();
        std::vector<uint8_t> serializedAttributes(attrSize);
        if (!attributes.SerializeToArray(serializedAttributes.data(), attrSize)) {
            spdlog::error("attributes SerializeToArray failure");
            break;
        }

        map = z_bytes_map_new();
        z_bytes_t bytes = {.len = serializedAttributes.size(), .start = serializedAttributes.data()};
        
        z_bytes_map_insert_by_alias(&map, z_bytes_new("attributes"), bytes);

        channel = new z_owned_reply_channel_t;
        if (nullptr == channel) {
            spdlog::error("failed to allocate channel");
            break;
        }

        *channel = zc_reply_fifo_new(16);

        opts.attachment = z_bytes_map_as_attachment(&map);

        if ((0 != payload.size()) && (nullptr != payload.data())) {
            opts.value.payload.len =  payload.size();
            opts.value.payload.start = payload.data();
        } else {
            opts.value.payload.len = 0;
            opts.value.payload.start = nullptr;
        }
        
        if (0 != z_get(z_loan(session_), z_keyexpr(std::to_string(uriHash).c_str()), "", z_move(channel->send), &opts)) {
            spdlog::error("z_get failure");
            break;
        }

        future = threadPool_->submit([=] { return handleReply(channel); });

        status = UCode::OK;
    
    } while(0);

    if (UCode::OK != status) {
        delete channel;
    }

    z_drop(&map);

    return future;
}


UMessage ZenohRpcClient::handleReply(z_owned_reply_channel_t *channel) {

    z_owned_reply_t reply = z_reply_null();

    UMessage message;

    while (z_call(channel->recv, &reply), z_check(reply)) {
        if (!z_reply_is_ok(&reply)) {
            spdlog::error("error received");
            break;
        }

        z_sample_t sample = z_reply_ok(&reply);

        if (sample.payload.len == 0 || sample.payload.start == nullptr) {
            spdlog::error("Payload is empty");
            break;
        }

        if (!z_check(sample.attachment)) {
            spdlog::error("No attachment found in the reply");
            break;
        }       

        z_bytes_t serializedAttributes = z_attachment_get(sample.attachment, z_bytes_new("attributes"));
        
        if ((0 == serializedAttributes.len) || (nullptr == serializedAttributes.start)) {
            spdlog::error("Serialized attributes not found in the attachment");
            break;
        }

        uprotocol::v1::UAttributes attributes;
        if (!attributes.ParseFromArray(serializedAttributes.start, serializedAttributes.len)) {
            spdlog::error("ParseFromArray failure");
            break;
        }

        auto payload = UPayload(sample.payload.start, sample.payload.len, UPayloadType::VALUE);

        message.setPayload(payload);
        message.setAttributes(attributes);

        z_drop(z_move(reply));
    }

    z_drop((z_owned_reply_channel_t*)channel);

    delete channel;

    return message;
}