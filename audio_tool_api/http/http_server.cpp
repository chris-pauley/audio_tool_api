//
//  http_server.cpp
//  audio_tool_api
//
//  Created by Chris Pauley on 3/12/19.
//  Copyright © 2019 Chris Pauley. All rights reserved.
//

#include "http_server.h"

void http_server::start(){
    auto const address = boost::asio::ip::make_address(config()->server_host);
    unsigned short const port = static_cast<unsigned short>(config()->server_port);
    tcp::endpoint endpoint(address,port);
    acceptor.emplace(ioc, endpoint);
    
    for (int x = 0; x < config()->num_threads; x++) {
        threads.emplace_back(q);
    }
    
    LOG(INFO) << "Listening on " << config()->server_host << ':' << config()->server_port;
    accept();
    
    for(auto & t : threads) {
        t.start();
    }
}

void http_server::accept(){
    
    active_connection = make_shared<tcp::socket>(acceptor->get_io_context());
    conn = make_shared<http_connection>(acceptor->get_io_context());
    // This assumes a maximum FD_SETSIZE of 1024 which is standard on most machines
    auto num = q.size_approx();
    DLOG_IF(INFO, num > 5) << "Approx Connections: " << q.size_approx();
    if(q.size_approx() > 1000){
        //this_thread::sleep_for(chrono::milliseconds(1));
        LOG(WARNING) << "Too Many connections in queue";
        return accept();
    }
    
    //acceptor->async_accept(*active_connection, [this](boost::beast::error_code err){
    acceptor->async_accept(conn->socket, [this](boost::beast::error_code err){
        if(err){
            LOG(ERROR) << "error accepting: " << err.message();
            stats()->increment("accept_errors");
            boost::asio::io_context::strand s(ioc);
            conn->socket.shutdown(tcp::socket::shutdown_both,err);
            s.wrap([this](){
                conn->socket.close();
            });
        } else {
            conn->accepted_time = chrono::steady_clock::now();
            auto diff = conn->created_time - conn->accepted_time;
            q.enqueue(std::move(conn));
            if(chrono::duration_cast<chrono::milliseconds>(diff).count() > 10000){
                LOG(ERROR) << "Connect Timeout";
            }
        }
        accept();
    });
}

void http_server::poll(){
    ioc.poll();
}

void http_server::run(){
    ioc.run();
}
