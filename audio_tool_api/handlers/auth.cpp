//
//  auth.cpp
//  audio_tool_api
//
//  Created by Chris Pauley on 1/9/19.
//  Copyright © 2019 Chris Pauley. All rights reserved.
//

#include "auth.h"

static size_t curl_write_func(void *contents, size_t size, size_t nmemb, void *userp){
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
};

/**
 This works until it doesn't. Need to make sure we're not hitting quotas and
 handle error responses due to those quotas accordingly
*/
string get_email(string token) {
    CURL *curl;
    CURLcode res;
    string response_body;
    curl = curl_easy_init();
    if(curl) {
        typedef size_t(*CURL_WRITEFUNCTION_PTR)(void*, size_t, size_t, void*);
        auto writefunc = [](void *contents, size_t size, size_t nmemb, void *userp){
            ((std::string*)userp)->append((char*)contents, size * nmemb);
            return size * nmemb;
        };
        struct curl_slist *chunk = nullptr;
        chunk = curl_slist_append(chunk, string("Authorization: Bearer " + token).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, "https://people.googleapis.com/v1/people/me?personFields=emailAddresses%2Cnames");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<CURL_WRITEFUNCTION_PTR>(writefunc));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        res = curl_easy_perform(curl);
        
        if(res != CURLE_OK){
            LOG(ERROR) << "curl_easy_perform() failed: " << curl_easy_strerror(res);
        }
        
        curl_easy_cleanup(curl);
    } else {
        LOG(ERROR) << "Curl did not initialize properly";
    }
    
    json people = json::parse(response_body);
    if(people.find("emailAddresses") != people.end()){
        for(auto &el : people["emailAddresses"].items()){
            if(el.value().find("value") != el.value().end()) {
                // Check email address in DB
                return el.value()["value"];
            } else {
                LOG(ERROR) << "Did not receive email despite proper Google OAuth Flow";
            }
        }
    } else {
        LOG(ERROR) << "Did not receive email despite proper Google OAuth Flow";
        if(people.find("error") != people.end()){
            LOG(ERROR) << people["error"].dump();
        }
    }
    return nullptr;
}

string get_token(string code){
    CURL *curl = curl_easy_init();
    CURLcode res;
    code = curl_easy_unescape(curl, code.c_str(), code.length(), NULL);
    string response_body;
    
    string location ="https://www.googleapis.com/oauth2/v4/token";
    string data = "code=" + Requests::url_encode(code);
    data += "&client_id=" + Requests::url_encode(config()->google_auth_client_id);
    data += "&client_secret=" + Requests::url_encode(config()->google_auth_secret);
    data += "&redirect_uri=https://audiotool.v3x.pw/api/auth";
    data += "&grant_type=authorization_code";
    
    if(curl) {
        struct curl_slist *chunk = nullptr;
        chunk = curl_slist_append(chunk, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl, CURLOPT_URL, location.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_write_func);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.length());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        res = curl_easy_perform(curl);
        
        if(res != CURLE_OK){
            LOG(ERROR) << "curl_easy_perform() failed: " << curl_easy_strerror(res);
        }
        
        curl_easy_cleanup(curl);
    } else {
        LOG(ERROR) << "Curl did not initialize properly";
    }
    
    json j = json::parse(response_body);
    if(j.find("access_token") != j.end()) {
        return j["access_token"];
    } else {
        LOG(ERROR) << "Did not receive access token during Google OAuth flow";
        throw runtime_error("Did not receive access token during Google OAuth flow");
        return ""; //nullptr;
    }
}

void auth_handler::get(http_request & request_data) {
    // This handler always forwards
    response.status = 302;
    
    bool couldNotAuth = false;
    if(request_data.has_param("error")) {
        // OAuth Error
        couldNotAuth = true;
    } else if(request_data.has_param("code")) {
        try {
            // OAuth Success
            string token = get_token(request_data.url_params["code"]);
            string email_address = get_email(token);
            user.fill_by_email(db, email_address);
            user.create_session(db);
            
            string location = "https://audiotool.v3x.pw/login/success/";
            string escaped_token;
            for(auto c : user.current_session.token) {
                if(c == '/') {
                    escaped_token += "%2F";
                } else if (c == '+') {
                    escaped_token += "%2B";
                } else if (c == '=') {
                    escaped_token += "%3D";
                } else {
                    escaped_token += c;
                }
            }
            location += escaped_token;
            response.headers.emplace("Location",location);
        } catch(std::runtime_error e) {
            couldNotAuth = true;
        }
        
    } else {
        couldNotAuth = true;
    }
    
    if(couldNotAuth){
        response.headers.emplace("Location","https://audiotool.v3x.pw/login/error/badAuth");
    }
}

void auth_handler::post(http_request & request_data) {
    nlohmann::json j = request_data.parse_json();
    string email = j["email"];
    string pass = j["password"];
    bool did_authenticate = user.authenticate(db,email,pass);
    if(did_authenticate) {
        user.create_session(db);
        response.headers.emplace("session-token", user.current_session.token);
        nlohmann::json user_data = user;
        response.set_content(user_data);
    } else {
        throw http_exception(403, "Authentication Failed");
    }
}

void auth_handler::put(http_request & request_data) {
    nlohmann::json j = request_data.parse_json();
    string email = j["email"];
    string pass = j["password"];
    user.create(db,email,pass);
    user.create_session(db);
    response.headers.emplace("session-token", user.current_session.token);
    nlohmann::json user_data = user;
    response.set_content(user_data);
    
}
