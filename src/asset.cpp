#include "asset.hpp"
#include <curl/curl.h>
#include <fstream>
#include "common/exceptions.hpp"

namespace judge {
using namespace std;

asset::asset(const string &name) : name(name) {}

local_asset::local_asset(const string &name, const filesystem::path &path)
    : asset(name), path(path) {}

void local_asset::fetch(const filesystem::path &path) {
    filesystem::copy(this->path, path / name);
}

text_asset::text_asset(const string &name, const string &text)
    : asset(name), text(text) {}

void text_asset::fetch(const filesystem::path &path) {
    ofstream fout(path / name);
    fout << text;
}

remote_asset::remote_asset(const string &name, const string &url_get)
    : asset(name), url(url_get) {}

// TODO: 针对 CURLcode throw 更加精确的 exception
void remote_asset::fetch(const filesystem::path &path) {
    auto filename = path / name;
    filesystem::create_directories(filename.parent_path());
    if (CURL *curl = curl_easy_init()) {
        FILE *fp = fopen(filename.c_str(), "wb");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        fclose(fp);
 
        if (res != CURLcode::CURLE_OK) {
            BOOST_THROW_EXCEPTION(network_error() << "unable to download " << url << ", CURLcode=" << res);
        }
    } else {
        BOOST_THROW_EXCEPTION(network_error() << "unable to download " << url << ", unable to init curl");
    }
}

}  // namespace judge
