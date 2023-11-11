#include "application.h"
#include "afv-native/Log.h"
#include "afv-native/atcClientWrapper.h"
#include "afv-native/event.h"
#include "config.h"
#include "data_file_handler.h"
#include "imgui.h"
#include "shared.h"
#include "style.h"
#include "util.h"
#include <SFML/Audio/Sound.hpp>
#include <SFML/Audio/SoundBuffer.hpp>
#include <SFML/Window/Joystick.hpp>
#include <httplib.h>
#include <memory>
#include <spdlog/spdlog.h>

namespace vector_audio::application {
using util::TextURL;

namespace afv_logger {
    void defaultLogger(const char* subsystem, const char* /*file*/,
        int /*line*/, const char* lineOut)
    {
        spdlog::info("[afv_native] {} {}", subsystem, lineOut);
    }

    afv_native::log_fn g_logger = defaultLogger;
}

App::App()
    : dataHandler_(std::make_unique<vatsim::DataHandler>())
{
    try {
        afv_native::api::atcClient::setLogger(afv_logger::g_logger);

        mClient_ = new afv_native::api::atcClient(shared::kClientName,
            vector_audio::Configuration::get_resource_folder().string());

        // Fetch all available devices on start
        vector_audio::shared::availableAudioAPI = mClient_->GetAudioApis();
        vector_audio::shared::availableInputDevices
            = mClient_->GetAudioInputDevices(vector_audio::shared::mAudioApi);
        vector_audio::shared::availableOutputDevices
            = mClient_->GetAudioOutputDevices(vector_audio::shared::mAudioApi);
        spdlog::debug("Created afv_native client.");
    } catch (std::exception& ex) {
        spdlog::critical(
            "Could not create AFV client interface: {}", ex.what());
        return;
    }

    // Load all from config
    try {
        using cfg = vector_audio::Configuration;

        vector_audio::shared::mOutputEffects
            = toml::find_or<bool>(cfg::config_, "audio", "vhf_effects", true);
        vector_audio::shared::mInputFilter
            = toml::find_or<bool>(cfg::config_, "audio", "input_filters", true);

        vector_audio::shared::vatsim_cid
            = toml::find_or<int>(cfg::config_, "user", "vatsim_id", 999999);
        vector_audio::shared::vatsim_password = toml::find_or<std::string>(
            cfg::config_, "user", "vatsim_password", std::string("password"));

        vector_audio::shared::keepWindowOnTop = toml::find_or<bool>(
            cfg::config_, "user", "keepWindowOnTop", false);

        vector_audio::shared::ptt = static_cast<sf::Keyboard::Scancode>(
            toml::find_or<int>(cfg::config_, "user", "ptt",
                static_cast<int>(sf::Keyboard::Scan::Unknown)));

        vector_audio::shared::joyStickId = static_cast<int>(
            toml::find_or<int>(cfg::config_, "user", "joyStickId", -1));
        vector_audio::shared::joyStickPtt = static_cast<int>(
            toml::find_or<int>(cfg::config_, "user", "joyStickPtt", -1));

        auto audio_providers = mClient_->GetAudioApis();
        vector_audio::shared::configAudioApi = toml::find_or<std::string>(
            cfg::config_, "audio", "api", std::string("Default API"));
        for (const auto& driver : audio_providers) {
            if (driver.second == vector_audio::shared::configAudioApi)
                vector_audio::shared::mAudioApi = driver.first;
        }

        vector_audio::shared::configInputDeviceName
            = toml::find_or<std::string>(
                cfg::config_, "audio", "input_device", std::string(""));
        vector_audio::shared::configOutputDeviceName
            = toml::find_or<std::string>(
                cfg::config_, "audio", "output_device", std::string(""));
        vector_audio::shared::configSpeakerDeviceName
            = toml::find_or<std::string>(
                cfg::config_, "audio", "speaker_device", std::string(""));
        vector_audio::shared::headsetOutputChannel
            = toml::find_or<int>(cfg::config_, "audio", "headset_channel", 0);

        vector_audio::shared::hardware = static_cast<afv_native::HardwareType>(
            toml::find_or<int>(cfg::config_, "audio", "hardware_type", 0));

        vector_audio::shared::apiServerPort
            = toml::find_or<int>(cfg::config_, "general", "api_port", 49080);
    } catch (toml::exception& exc) {
        spdlog::error(
            "Failed to parse available configuration: {}", exc.what());
    }

    // Bind the callbacks from the client
    // std::bind(&App::_eventCallback, this, std::placeholders::_1,
    // std::placeholders::_2, std::placeholders::_3)
    mClient_->RaiseClientEvent(
        [this](auto&& event_type, auto&& data_one, auto&& data_two) {
            eventCallback(std::forward<decltype(event_type)>(event_type),
                std::forward<decltype(data_one)>(data_one),
                std::forward<decltype(data_two)>(data_two));
        });

    // Start the API timer
    shared::currentlyTransmittingApiTimer
        = std::chrono::high_resolution_clock::now();

    // Start the SDK server
    buildSDKServer();

    // Load the airport database async
    std::thread(&vector_audio::application::App::loadAirportsDatabaseAsync)
        .detach();

    auto sound_path = Configuration::get_resource_folder()
        / std::filesystem::path("disconnect.wav");

    if (!disconnect_warning_soundbuffer_.loadFromFile(sound_path.string())) {
        disconnect_warning_sound_available = false;
        spdlog::error(
            "Could not load warning sound file, disconnection will be silent");
    }

    sound_player_.setBuffer(disconnect_warning_soundbuffer_);
}

App::~App() { delete mClient_; }

void App::loadAirportsDatabaseAsync()
{
    // if we cannot load this database, it's not that important, we will just
    // log it.

    if (!std::filesystem::exists(
            vector_audio::Configuration::airports_db_file_path_)) {
        spdlog::warn("Could not find airport database json file");
        return;
    }

    try {
        // We do performance analysis here
        auto t1 = std::chrono::high_resolution_clock::now();
        std::ifstream f(vector_audio::Configuration::airports_db_file_path_);
        nlohmann::json data = nlohmann::json::parse(f);

        // Loop through all the icaos
        for (const auto& obj : data.items()) {
            ns::Airport ar;
            obj.value().at("icao").get_to(ar.icao);
            obj.value().at("elevation").get_to(ar.elevation);
            obj.value().at("lat").get_to(ar.lat);
            obj.value().at("lon").get_to(ar.lon);

            // Assumption: The user will not have time to connect by the time
            // this is loaded, hence should be fine re concurrency
            ns::Airport::All.insert(std::make_pair(obj.key(), ar));
        }

        auto t2 = std::chrono::high_resolution_clock::now();
        spdlog::info("Loaded {} airports in {}", ns::Airport::All.size(),
            std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1));
    } catch (nlohmann::json::exception& ex) {
        spdlog::warn("Could parse airport database: {}", ex.what());
        return;
    }
}

void App::buildSDKServer()
{
    try {
        mSDKServer_ = restinio::run_async<>(restinio::own_io_context(),
            restinio::server_settings_t<> {}
                .port(vector_audio::shared::apiServerPort)
                .address("0.0.0.0")
                .request_handler([&](auto req) {
                    if (restinio::http_method_get() == req->header().method()
                        && req->header().request_target() == "/transmitting") {

                        const std::lock_guard<std::mutex> lock(
                            vector_audio::shared::transmitting_mutex);
                        return req->create_response()
                            .set_body(vector_audio::shared::
                                    currentlyTransmittingApiData)
                            .done();
                    }
                    if (restinio::http_method_get() == req->header().method()
                        && req->header().request_target() == "/rx") {
                        std::vector<shared::StationElement> bar;

                        // copy only positive numbers:
                        std::copy_if(shared::FetchedStations.begin(),
                            shared::FetchedStations.end(),
                            std::back_inserter(bar),
                            [this](const shared::StationElement& s) {
                                if (!mClient_->IsVoiceConnected())
                                    return false;
                                return mClient_->GetRxState(s.freq);
                            });

                        std::string out;
                        if (!bar.empty()) {
                            for (auto& f : bar) {
                                out += f.callsign + ":" + f.human_freq + ",";
                            }
                        }

                        if (out.back() == ',') {
                            out.pop_back();
                        }

                        return req->create_response().set_body(out).done();
                    }
                    if (restinio::http_method_get() == req->header().method()
                        && req->header().request_target() == "/tx") {
                        std::vector<shared::StationElement> bar;

                        // copy only positive numbers:
                        std::copy_if(shared::FetchedStations.begin(),
                            shared::FetchedStations.end(),
                            std::back_inserter(bar),
                            [this](const shared::StationElement& s) {
                                if (!mClient_->IsVoiceConnected())
                                    return false;
                                return mClient_->GetTxState(s.freq);
                            });

                        std::string out;
                        if (!bar.empty()) {
                            for (auto& f : bar) {
                                out += f.callsign + ":" + f.human_freq + ",";
                            }
                        }

                        if (out.back() == ',') {
                            out.pop_back();
                        }

                        return req->create_response().set_body(out).done();
                    }

                    return req->create_response()
                        .set_body(vector_audio::shared::kClientName)
                        .done();
                }),
            16U);
    } catch (std::exception& ex) {
        spdlog::error("Failed to created SDK http server, is the port in use?");
        spdlog::error("%{}", ex.what());
    }
}

void App::eventCallback(
    afv_native::ClientEventType evt, void* data, void* data2)
{
    switch (evt) {
    case afv_native::ClientEventType::VccsReceived: {
        if (data != nullptr && data2 != nullptr) {
            // We got new VCCS stations, we can add them to our list and start
            // getting their transceivers
            std::map<std::string, unsigned int> stations
                = *reinterpret_cast<std::map<std::string, unsigned int>*>(
                    data2);

            if (mClient_->IsVoiceConnected()) {
                for (auto s : stations) {
                    if (!util::isValid8_33kHzChannel(s.second)) {
                        s.second = util::round8_33kHzChannel(s.second);
                    }
                    shared::StationElement el
                        = shared::StationElement::build(s.first, s.second);

                    if (!frequencyExists(el.freq))
                        shared::FetchedStations.push_back(el);
                }
            }
        }

        break;
    }

    case afv_native::ClientEventType::StationTransceiversUpdated: {
        if (data != nullptr) {
            // We just refresh the transceiver count in our display
            std::string station = *reinterpret_cast<std::string*>(data);
            auto it = std::find_if(shared::FetchedStations.begin(),
                shared::FetchedStations.end(),
                [station](const auto& fs) { return fs.callsign == station; });
            if (it != shared::FetchedStations.end())
                it->transceivers
                    = mClient_->GetTransceiverCountForStation(station);
        }

        break;
    }

    case afv_native::ClientEventType::APIServerError: {
        // We got an error from the API server, we can display this to the user
        if (data != nullptr) {
            afv_native::afv::APISessionError err
                = *reinterpret_cast<afv_native::afv::APISessionError*>(data);

            if (err == afv_native::afv::APISessionError::BadPassword
                || err
                    == afv_native::afv::APISessionError::RejectedCredentials) {
                errorModal("Could not login to VATSIM.\nInvalid "
                           "Credentials.\nCheck your password/cid!");

                spdlog::error("Got invalid credential errors from AFV API: "
                              "HTTP 403 or 401");
            }

            if (err == afv_native::afv::APISessionError::ConnectionError) {
                errorModal("Could not login to VATSIM.\nConnection "
                           "Error.\nCheck your internet connection.");

                spdlog::error("Got connection error from AFV API: local socket "
                              "or curl error");
                disconnectAndCleanup();
                playErrorSound();
            }

            if (err
                == afv_native::afv::APISessionError::
                    BadRequestOrClientIncompatible) {
                errorModal("Could not login to VATSIM.\n Bad Request or Client "
                           "Incompatible.");

                spdlog::error("Got connection error from AFV API: HTTP 400 - "
                              "Bad Request or Client Incompatible");
                disconnectAndCleanup();
                playErrorSound();
            }

            if (err == afv_native::afv::APISessionError::InvalidAuthToken) {
                errorModal("Could not login to VATSIM.\n Invalid Auth Token.");

                spdlog::error("Got connection error from AFV API: Invalid Auth "
                              "Token Local Parse Error.");
                disconnectAndCleanup();
                playErrorSound();
            }

            if (err
                == afv_native::afv::APISessionError::
                    AuthTokenExpiryTimeInPast) {
                errorModal("Could not login to VATSIM.\n Auth Token has "
                           "expired.\n Check your system clock.");

                spdlog::error("Got connection error from AFV API: Auth Token "
                              "Expiry in the past");
                disconnectAndCleanup();
                playErrorSound();
            }

            if (err == afv_native::afv::APISessionError::OtherRequestError) {
                errorModal("Could not login to VATSIM.\n Unknown Error.");

                spdlog::error(
                    "Got connection error from AFV API: Unknown Error");

                disconnectAndCleanup();
                playErrorSound();
            }
        }

        break;
    }

    case afv_native::ClientEventType::AudioError: {
        errorModal("Error starting audio devices.\nPlease check "
                   "your log file for details.\nCheck your audio config!");
        disconnectAndCleanup();

        break;
    }

    case afv_native::ClientEventType::VoiceServerDisconnected: {

        if (!manuallyDisconnected_) {
            playErrorSound();
        }

        manuallyDisconnected_ = false;
        // disconnectAndCleanup();

        break;
    }

    case afv_native::ClientEventType::VoiceServerError: {
        int err_code = *reinterpret_cast<int*>(data);
        errorModal("Voice server returned error " + std::to_string(err_code)
            + ", please check the log file.");
        disconnectAndCleanup();
        playErrorSound();

        break;
    }

    case afv_native::ClientEventType::VoiceServerChannelError: {
        int err_code = *reinterpret_cast<int*>(data);
        errorModal("Voice server returned channel error "
            + std::to_string(err_code) + ", please check the log file.");
        disconnectAndCleanup();
        playErrorSound();

        break;
    }

    case afv_native::ClientEventType::AudioDeviceStoppedError: {
        errorModal("The audio device " + *reinterpret_cast<std::string*>(data)
            + " has stopped working "
              ", check if they are still physically connected.");
        disconnectAndCleanup();
        playErrorSound();

        break;
    }

    case afv_native::ClientEventType::StationDataReceived: {
        if (data != nullptr && data2 != nullptr) {
            // We just refresh the transceiver count in our display
            bool found = *reinterpret_cast<bool*>(data);
            if (found) {
                auto station
                    = *reinterpret_cast<std::pair<std::string, unsigned int>*>(
                        data2);

                station.second = util::cleanUpFrequency(station.second);

                shared::StationElement el = shared::StationElement::build(
                    station.first, station.second);

                if (!frequencyExists(el.freq))
                    shared::FetchedStations.push_back(el);
            } else {
                errorModal("Could not find station in database.");
                spdlog::warn(
                    "Station not found in AFV database through search");
            }
        }

        break;
    }

    case afv_native::ClientEventType::RxClosed: {
        if (data != nullptr)
            shared::last_rx_close = *reinterpret_cast<unsigned int*>(data);

        break;
    }
    }
}

// Main loop
void App::render_frame()
{
    // AFV stuff
    if (mClient_) {
        vector_audio::shared::mPeak = mClient_->GetInputPeak();
        vector_audio::shared::mVu = mClient_->GetInputVu();

        // Set the Ptt if required, input based on event
        if (mClient_->IsVoiceConnected()
            && (shared::ptt != sf::Keyboard::Scan::Unknown
                || shared::joyStickId != -1)) {
            if (shared::isPttOpen) {
                if (shared::joyStickId != -1) {
                    if (!sf::Joystick::isButtonPressed(
                            shared::joyStickId, shared::joyStickPtt)) {
                        shared::isPttOpen = false;
                    }
                } else {
                    if (!sf::Keyboard::isKeyPressed(shared::ptt)) {
                        shared::isPttOpen = false;
                    }
                }

                mClient_->SetPtt(shared::isPttOpen);
            } else {
                if (shared::joyStickId != -1) {
                    if (sf::Joystick::isButtonPressed(
                            shared::joyStickId, shared::joyStickPtt)) {
                        shared::isPttOpen = true;
                    }
                } else {
                    if (sf::Keyboard::isKeyPressed(shared::ptt)) {
                        shared::isPttOpen = true;
                    }
                }

                mClient_->SetPtt(shared::isPttOpen);
            }
        }

        if (mClient_->IsAPIConnected() && shared::FetchedStations.empty()
            && !shared::bootUpVccs) {
            // We force add the current user frequency
            shared::bootUpVccs = true;

            // We replaced double _ which may be used during frequency
            // handovers, but are not defined in database
            std::string clean_callsign = vector_audio::util::ReplaceString(
                shared::session::callsign, "__", "_");

            shared::StationElement el = shared::StationElement::build(
                clean_callsign, shared::session::frequency);
            if (!frequencyExists(el.freq))
                shared::FetchedStations.push_back(el);

            this->mClient_->AddFrequency(
                shared::session::frequency, clean_callsign);
            mClient_->SetEnableInputFilters(vector_audio::shared::mInputFilter);
            mClient_->SetEnableOutputEffects(
                vector_audio::shared::mOutputEffects);
            this->mClient_->UseTransceiversFromStation(
                clean_callsign, shared::session::frequency);
            this->mClient_->SetRx(shared::session::frequency, true);
            if (shared::session::facility > 0) {
                this->mClient_->SetTx(shared::session::frequency, true);
                this->mClient_->SetXc(shared::session::frequency, true);
            }
            this->mClient_->FetchStationVccs(clean_callsign);
            mClient_->SetRadiosGain(shared::RadioGain / 100.0F);
        }
    }

    // The live Received callsign data
    std::vector<std::string> received_callsigns;
    std::vector<std::string> live_received_callsigns;

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("MainWindow", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoScrollWithMouse
            | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Calculate various sizes for layout

    ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImVec2 margin(2.f * spacing.x, 2.f * spacing.y);

    ImVec2 win_size = ImGui::GetWindowSize();
    ImVec2 inner_size(win_size.x - 2.f * margin.x, win_size.y - 2.f * margin.y);
    ImVec2 char_size = ImGui::CalcTextSize("0");

#define CALC_SIZE(name, cx, px, sx, cy, py, sy) \
    ImVec2 name##_size(cx * char_size.x + px * padding.x + sx * spacing.x, \
                       cy * char_size.y + py * padding.y + sy * spacing.y)
    CALC_SIZE(ctrls_buttons,  10.f, 2.f, 0.f,  2.f, 4.f, 1.f);
    CALC_SIZE(ctrls_statuses, 21.f, 0.f, 1.f,  3.f, 0.f, 0.f);
    CALC_SIZE(ctrls_lights,   4.f,  4.f, 0.5f, 1.f, 2.f, 0.f);
    CALC_SIZE(ctrls_lights_x, 12.f, 4.f, 0.5f, 1.f, 2.f, 0.f);
    CALC_SIZE(ctrls_add,      16.f, 4.f, 0.5f, 1.f, 2.f, 0.f);
    CALC_SIZE(ctrls_client,   12.f, 0.f, 0.f,  3.f, 0.f, 0.f);

    CALC_SIZE(radio_block,  12.f, 2.f, 0.f, 3.f, 2.f, 0.f);
    CALC_SIZE(radio_button, 2.f,  2.f, 0.f, 1.f, 2.f, 0.f);
#undef CALC_SIZE

    bool collapse = false;
    int ctrls_cols = 1;

    ImVec2 ctrls_margin = margin;
    ImVec2 ctrls_size(
        std::max(ctrls_lights_size.x, ctrls_add_size.x),
        std::max(ctrls_buttons_size.y, ctrls_statuses_size.y)
    );

    if (win_size.y < ctrls_size.y + 0.5f * radio_block_size.y + 4.f * margin.y) {
        collapse = true;
        ctrls_margin.y = std::max(0.f, floor((win_size.y - ctrls_size.y) / 2.f));
        ctrls_size.x = ctrls_lights_x_size.x;
    }

    if (ctrls_client_size.x + spacing.x < inner_size.x - ctrls_size.x) {
        ctrls_cols++;
        ctrls_size.x += ctrls_client_size.x + spacing.x;

        if (ctrls_statuses_size.x + spacing.x < inner_size.x - ctrls_size.x) {
            ctrls_cols++;
            ctrls_size.x += ctrls_statuses_size.x + spacing.x;

            if (ctrls_buttons_size.x + spacing.x < inner_size.x - ctrls_size.x) {
                ctrls_cols++;
                ctrls_size.x += ctrls_buttons_size.x + spacing.x;
            }
        }
    }

    float ctrls_spacing = (inner_size.x - ctrls_size.x) / (float) (ctrls_cols - 1);
    ctrls_spacing = std::min(ctrls_spacing, 5.f * spacing.x);
    ctrls_size.x += ctrls_spacing * (float) (ctrls_cols - 1);
    ctrls_spacing += spacing.x;

    float ctrls_add_extra = std::min(48.f, inner_size.x - ctrls_size.x);
    ctrls_size.x += ctrls_add_extra;
    ctrls_add_size.x += ctrls_add_extra;

    float sbw = ImGui::GetStyle().ScrollbarSize;
    float radio_width = radio_block_size.x + 2.f * radio_button_size.x + spacing.x;
    int radio_cols = (inner_size.x + spacing.x - sbw) / (radio_width + spacing.x);
    ImVec2 radio_size(
        floor(std::min(
            1.5f * radio_width,
            ((inner_size.x + spacing.x - sbw) / (float) radio_cols) - spacing.x
        )),
        std::max(radio_block_size.y, 2.f * radio_button_size.y + spacing.y / 2.f)
    );

    bool show_settings = false;

    // Top half

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 1.f));

    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ctrls_margin);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ctrls_spacing, 0.f));
        ImGui::BeginChild(
            "ctrls",
            ImVec2(
                ctrls_size.x + 2.f * ctrls_margin.x,
                ctrls_size.y + 2.f * ctrls_margin.y
            ),
            false, ImGuiWindowFlags_AlwaysUseWindowPadding
        );

        if (ctrls_cols >= 4) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
            ImGui::BeginGroup();

            // Connect button logic
            if (!mClient_->IsVoiceConnected() && !mClient_->IsAPIConnected()) {
                bool ready_to_connect = (!shared::session::is_connected
                                            && dataHandler_->isSlurperAvailable())
                    || shared::session::is_connected;
                style::push_disabled_on(!ready_to_connect);

                if (ImGui::Button("Connect", ImVec2(ctrls_buttons_size.x, 0.f))) {

                    if (!vector_audio::shared::session::is_connected
                        && dataHandler_->isSlurperAvailable()) {
                        // We manually call the slurper here in case that we do not have
                        // a connection yet Although this will block the whole program,
                        // it is not an issue in this case As the user does not need to
                        // interact with the software while we attempt A connection that
                        // fails once will not be retried and will default to datafile
                        // only

                        vector_audio::shared::session::is_connected
                            = dataHandler_->getConnectionStatusWithSlurper();
                    }

                    if (vector_audio::shared::session::is_connected) {
                        if (mClient_->IsAudioRunning()) {
                            mClient_->StopAudio();
                        }
                        if (mClient_->IsAPIConnected()) {
                            mClient_->Disconnect(); // Force a disconnect of API
                        }

                        mClient_->SetAudioApi(findAudioAPIorDefault());
                        mClient_->SetAudioInputDevice(
                            findHeadsetInputDeviceOrDefault());
                        mClient_->SetAudioOutputDevice(
                            findHeadsetOutputDeviceOrDefault());
                        mClient_->SetAudioSpeakersOutputDevice(
                            findSpeakerOutputDeviceOrDefault());
                        mClient_->SetHardware(vector_audio::shared::hardware);
                        mClient_->SetHeadsetOutputChannel(
                            vector_audio::shared::headsetOutputChannel);

                        if (!dataHandler_->isSlurperAvailable()) {
                            std::string client_icao
                                = vector_audio::shared::session::callsign.substr(0,
                                    vector_audio::shared::session::callsign.find('_'));
                            // We use the airport database for this
                            if (ns::Airport::All.find(client_icao)
                                != ns::Airport::All.end()) {
                                auto client_airport = ns::Airport::All.at(client_icao);

                                // We pad the elevation by 10 meters to simulate the
                                // client being in a tower
                                mClient_->SetClientPosition(client_airport.lat,
                                    client_airport.lon, client_airport.elevation + 33,
                                    client_airport.elevation + 33);

                                spdlog::info("Found client position in database at "
                                            "lat:{}, lon:{}, elev:{}",
                                    client_airport.lat, client_airport.lon,
                                    client_airport.elevation);
                            } else {
                                spdlog::warn(
                                    "Client position is unknown, setting default.");

                                // Default position is over Paris somewhere
                                mClient_->SetClientPosition(
                                    48.967860, 2.442000, 300, 300);
                            }
                        } else {
                            spdlog::info(
                                "Found client position from slurper at lat:{}, lon:{}",
                                vector_audio::shared::session::latitude,
                                vector_audio::shared::session::longitude);
                            mClient_->SetClientPosition(
                                vector_audio::shared::session::latitude,
                                vector_audio::shared::session::longitude, 300, 300);
                        }

                        mClient_->SetCredentials(
                            std::to_string(vector_audio::shared::vatsim_cid),
                            vector_audio::shared::vatsim_password);
                        mClient_->SetCallsign(vector_audio::shared::session::callsign);
                        mClient_->SetRadiosGain(shared::RadioGain / 100.0F);
                        if (!mClient_->Connect()) {
                            spdlog::error(
                                "Failed to connect: afv_lib says API is connected.");
                        };
                    } else {
                        errorModal("Not connected to VATSIM!");
                    }
                }
                style::pop_disabled_on(!ready_to_connect);
            } else {
                style::PushFrameStyle(style::FrameType::FrameSelected);

                // Auto disconnect if we need
                auto pressed_disconnect =
                    ImGui::Button("Disconnect", ImVec2(ctrls_buttons_size.x, 0.f));
                if (pressed_disconnect || !shared::session::is_connected) {

                    if (pressed_disconnect) {
                        manuallyDisconnected_ = true;
                    }

                    disconnectAndCleanup();
                }
                style::PopFrameStyle();
            }

            // Settings button
            style::push_disabled_on(mClient_->IsAPIConnected());
            if (
                ImGui::Button("Settings", ImVec2(ctrls_buttons_size.x, 0.f)) &&
                !mClient_->IsAPIConnected()
            ) show_settings = true;
            style::pop_disabled_on(mClient_->IsAPIConnected());

            ImGui::PopStyleVar();
            ImGui::EndGroup();
            ImGui::SameLine(0.f, ctrls_spacing);
        }

        if (ctrls_cols >= 3) {
            ImGui::PushStyleVar(
                ImGuiStyleVar_CellPadding, ImVec2(spacing.x / 2.f, 0.f)
            );
            ImGui::BeginTable(
                "ctrls_statuses", 2,
                ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_SizingFixedFit,
                ImVec2(ctrls_statuses_size.x, 0.f)
            );

            // Align table to row centre
            ImGui::TableNextRow(
                ImGuiTableRowFlags_None,
                (ctrls_size.y - ctrls_statuses_size.y) / 2.f
            );
            ImGui::TableNextRow();

            // Callsign display
            ImGui::TableNextColumn();
            ImGui::Text("Callsign");
            ImGui::TableNextColumn();
            style::PushTextStyle(
                shared::session::callsign == "No connection"
                    ? style::TextNormal
                    : style::TextBright
            );
            ImGui::TextUnformatted(shared::session::callsign.c_str());
            style::PopTextStyle();

            // API/voice connection status
            ImGui::TableNextColumn();
            ImGui::Text("  Status");
            ImGui::TableNextColumn();
            if (mClient_->IsAPIConnected() && mClient_->IsVoiceConnected()) {
                style::PushTextStyle(style::TextSuccess);
                ImGui::Text("Connected");
            } else {
                style::PushTextStyle(style::TextFailure);
                ImGui::Text(
                    mClient_->IsAPIConnected()
                        ? "No voice"
                        : mClient_->IsVoiceConnected()
                            ? "No API"
                            : "Not connected"
                );
            }
            style::PopTextStyle();

            // Datasource information
            ImGui::TableNextColumn();
            ImGui::Text(" Sources");
            ImGui::TableNextColumn();
            if (dataHandler_->isSlurperAvailable()) {
                style::PushTextStyle(style::TextSuccess);
                ImGui::Text("Slurper");
            } else if (dataHandler_->isDatafileAvailable()) {
                style::PushTextStyle(style::TextNormal);
                ImGui::Text("Datafile");
            } else {
                style::PushTextStyle(style::TextFailure);
                ImGui::Text("No data");
            }
            style::PopTextStyle();
            ImGui::SameLine(0.f, spacing.x);
            util::HelpMarker(
                "The data source where VectorAudio\n"
                "checks for your VATSIM connection.\n"
                "\"No data\" means that the VATSIM\n"
                "servers could not be reached."
            );

            ImGui::PopStyleVar();
            ImGui::EndTable();
            ImGui::SameLine(0.f, ctrls_spacing);
        }

        if (ctrls_cols >= 1) {
            /* auto rx_style = std::any_of(
                shared::FetchedStations.front(), shared::FetchedStations.back(),
                [this](auto &el) { return mClient_->GetRxActive(el.freq); }
            ) ? style::FrameRadio : style::FrameNormal;
            auto tx_style = std::any_of(
                shared::FetchedStations.front(), shared::FetchedStations.back(),
                [this](auto &el) { return mClient_->GetTxActive(el.freq); }
            ) ? style::FrameRadio : style::FrameNormal; */

            std::string last_rx = shared::last_rx_close
                ? mClient_->LastTransmitOnFreq(shared::last_rx_close)
                : "Inactive";

            bool rx = false, tx = false;
            for (auto &el : shared::FetchedStations) {
                if (mClient_->GetRxActive(el.freq)) {
                    last_rx = mClient_->LastTransmitOnFreq(el.freq);
                    rx = true;
                }

                tx = tx || mClient_->GetTxActive(el.freq);
            }

            auto rx_style = rx ? style::FrameRadio : style::FrameNormal;
            auto tx_style = tx ? style::FrameRadio : style::FrameNormal;

            last_rx = last_rx.substr(0, std::min(10, (int) last_rx.size()));

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
            ImGui::BeginGroup();

            ImVec2 light_size(
                (ctrls_lights_size.x - 0.5f * spacing.x) / 2.f,
                ctrls_lights_size.y
            );

            if (collapse) {
                style::PushFrameStyle(rx_style, false);
                ImGui::Button(
                    last_rx.append("##RXc").c_str(),
                    ImVec2(
                        ctrls_lights_x_size.x - light_size.x - 0.5f * spacing.x,
                        light_size.y
                    )
                );
            } else {
                style::PushFrameStyle(rx_style, false);
                ImGui::Button("RX", light_size);
            }

            style::UnroundCorners(0b0110, true);
            ImGui::SameLine(0.f, 0.5f * spacing.x);
            style::PopFrameStyle();

            style::PushFrameStyle(tx_style, false);
            ImGui::Button("TX", light_size);
            style::UnroundCorners(0b1001, true);
            style::PopFrameStyle();

            if (collapse) {
                ImGui::SetNextItemWidth(ctrls_lights_x_size.x);
            } else {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(
                    ctrls_add_size.x - ctrls_lights_size.x - spacing.x
                );
            }

            style::push_disabled_on(!mClient_->IsVoiceConnected());

            if (
                ImGui::SliderInt("##gain", &shared::RadioGain, 0, 200, "%3i%%") &&
                mClient_->IsVoiceConnected()
            ) mClient_->SetRadiosGain(shared::RadioGain / 100.0F);

            if (!collapse) {
                ImGui::SetNextItemWidth(0.75f * ctrls_add_size.x - 0.5f * spacing.x);
                bool input = ImGui::InputTextWithHint(
                    "##callsign", "Callsign...", &shared::station_auto_add_callsign,
                    ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll |
                        ImGuiInputTextFlags_CharsUppercase
                );
                style::UnroundCorners(0b0110, false, false);
                ImGui::SameLine(0.f, 0.5f * spacing.x);

                bool button = ImGui::Button("Add", ImVec2(ctrls_add_size.x / 4.f, 0.f));
                style::UnroundCorners(0b1001, true);

                if ((input || button) && mClient_->IsVoiceConnected()) {
                    if (!util::startsWith(shared::station_auto_add_callsign, "!")) {
                        mClient_->GetStation(shared::station_auto_add_callsign);
                        mClient_->FetchStationVccs(shared::station_auto_add_callsign);
                    } else {
                        double latitude, longitude;
                        shared::station_auto_add_callsign
                            = shared::station_auto_add_callsign.substr(1);

                        if (!frequencyExists(shared::kUnicomFrequency)) {
                            if (dataHandler_->getPilotPositionWithAnything(
                                    shared::station_auto_add_callsign, latitude,
                                    longitude)) {

                                shared::StationElement el
                                    = shared::StationElement::build(
                                        shared::station_auto_add_callsign,
                                        shared::kUnicomFrequency);

                                shared::FetchedStations.push_back(el);
                                mClient_->SetClientPosition(
                                    latitude, longitude, 1000, 1000);
                                mClient_->AddFrequency(shared::kUnicomFrequency,
                                    shared::station_auto_add_callsign);
                                mClient_->SetRx(shared::kUnicomFrequency, true);
                                mClient_->SetRadiosGain(shared::RadioGain / 100.0F);

                            } else {
                                errorModal("Could not find pilot connected under that "
                                            "callsign.");
                            }
                        } else {
                            errorModal("Another UNICOM frequency is active, please "
                                        "delete it first.");
                        }
                    }

                    shared::station_auto_add_callsign = "";
                }
            }

            style::pop_disabled_on(!mClient_->IsVoiceConnected());

            ImGui::PopStyleVar();
            ImGui::EndGroup();
            ImGui::SameLine(0.f, ctrls_spacing);
        }

        if (ctrls_cols >= 2) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.f, 0.f));
            ImGui::BeginTable(
                "ctrls_client", 1, ImGuiTableFlags_SizingFixedFit,
                ImVec2(ctrls_client_size.x, 0.f)
            );

            // Align table to row centre
            ImGui::TableNextRow(
                ImGuiTableRowFlags_None,
                (ctrls_size.y - ctrls_client_size.y) / 2.f
            );
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("VectorAudio");
            ImGui::TableNextColumn();
            ImGui::Text(VECTOR_VERSION);
            ImGui::TableNextColumn();
            TextURL(
                "Licenses",
                (vector_audio::Configuration::get_resource_folder() / "LICENSE.txt")
                    .string()
            );

            ImGui::PopStyleVar();
            ImGui::EndTable();
        }

        ImGui::PopStyleVar(2);
        ImGui::EndChild();
    }

    // Bottom half

    if (!collapse) {
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, 1.f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, margin);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ctrls_spacing, 0.f));
        ImGui::BeginChild(
            "radio",
            ImVec2(
                win_size.x,
                win_size.y - ctrls_size.y - 2.f * ctrls_margin.y - 4.f
            ),
            false, ImGuiWindowFlags_AlwaysUseWindowPadding
        );

        if (shared::FetchedStations.empty()) ImGui::Text("No stations added");

        ImGui::PushStyleVar(
            ImGuiStyleVar_CellPadding,
            ImVec2(spacing.x / 2.f, spacing.y / 2.f)
        );
        ImGui::BeginTable(
            "radios", std::max(radio_cols, 1),
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX
        );

        float radio_content = radio_size.x - spacing.x;
        float radio_unit_total = radio_block_size.x + 2.f * radio_button_size.x;

        ImVec2 radio_block(
            radio_content * radio_block_size.x / radio_unit_total,
            radio_size.y
        );
        ImVec2 radio_button(
            radio_content * radio_button_size.x / radio_unit_total,
            (radio_size.y - spacing.y / 2.f) / 2.f
        );

        for (auto& el : shared::FetchedStations) {
            ImGui::TableNextColumn();

            // Polling all data
            bool rx_state = mClient_->GetRxState(el.freq);
            bool rx_active = mClient_->GetRxActive(el.freq);
            bool tx_state = mClient_->GetTxState(el.freq);
            bool tx_active = mClient_->GetTxActive(el.freq);
            bool xc_state = mClient_->GetXcState(el.freq);
            bool is_on_speaker = !mClient_->GetOnHeadset(el.freq);
            bool freq_active = mClient_->IsFrequencyActive(el.freq)
                && (rx_state || tx_state || xc_state);

            {
                // Label block

                ImVec2 origin = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    origin,
                    ImVec2(
                        origin.x + (radio_cols ? radio_block.x : inner_size.x),
                        origin.y + radio_block.y
                    ),
                    style::frame_normal[
                        freq_active ? style::FrameSelected : style::FrameNormal
                    ],
                    ImGui::GetStyle().FrameRounding,
                    radio_cols ? ImDrawFlags_RoundCornersLeft : 0
                );

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
                ImGui::BeginChild(
                    std::string("radio_block-").append(el.callsign).c_str(),
                    radio_cols ? radio_block : ImVec2(inner_size.x, radio_block.y),
                    false, ImGuiWindowFlags_AlwaysUseWindowPadding
                );

                if (ImGui::BeginPopupContextWindow()) {
                    style::PushTextStyle(style::TextBright);
                    ImGui::Text("Station options");
                    style::PopTextStyle();

                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, 1.f);

                    if (
                        ImGui::Selectable(
                            std::string("Refresh transceivers##")
                                .append(el.callsign)
                                .c_str()
                        )
                    ) {
                        mClient_->FetchTransceiverInfo(el.callsign);
                    }

                    if (
                        ImGui::Selectable(
                            std::string("Remove station##")
                                .append(el.callsign)
                                .c_str()
                        )
                    ) {
                        mClient_->RemoveFrequency(el.freq);

                        shared::FetchedStations.erase(
                            std::remove_if(
                                shared::FetchedStations.begin(),
                                shared::FetchedStations.end(),
                                [el](shared::StationElement const& p) {
                                    return el.freq == p.freq;
                                }
                            ),
                            shared::FetchedStations.end()
                        );
                    }

                    ImGui::EndPopup();
                }

                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2());
                if (ImGui::BeginTable(
                    std::string("radio_labels-").append(el.callsign).c_str(),
                    1, ImGuiTableFlags_SizingFixedFit
                )) {
                    ImGui::TableNextColumn();
                    style::PushTextStyle(style::TextBright);
                    ImGui::TextUnformatted(el.callsign.c_str());
                    style::PopTextStyle();

                    ImGui::TableNextColumn();
                    if (freq_active && el.transceivers >= 0)
                        ImGui::Text("%s (%d)", el.human_freq.c_str(), el.transceivers);
                    else
                        ImGui::TextUnformatted(el.human_freq.c_str());

                    std::string last_rx = mClient_->LastTransmitOnFreq(el.freq);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        !last_rx.empty() && rx_state
                            ? last_rx.c_str()
                            : "Inactive"
                    );

                    ImGui::EndTable();
                }

                ImGui::PopStyleVar(2);
                ImGui::EndChild();
                ImGui::SameLine(0.f, spacing.x / 2.f);
            }

            if (radio_cols) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, spacing.y / 2.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
                ImGui::BeginGroup();

                // RX button

                if (rx_state) rx_active
                    ? style::PushFrameStyle(style::FrameRadio)
                    : style::PushFrameStyle(style::FrameSelected);

                if (
                    ImGui::Button(
                        std::string("RX##").append(el.callsign).c_str(),
                        radio_button
                    )
                ) {
                    if (freq_active) {
                        mClient_->SetRx(el.freq, !rx_state);
                    } else {
                        mClient_->AddFrequency(el.freq, el.callsign);
                        mClient_->SetEnableInputFilters(shared::mInputFilter);
                        mClient_->SetEnableOutputEffects(shared::mOutputEffects);
                        mClient_->UseTransceiversFromStation(el.callsign, el.freq);
                        mClient_->SetRx(el.freq, true);
                        mClient_->SetRadiosGain(shared::RadioGain / 100.0F);
                    }
                }

                if (rx_state) style::PopFrameStyle();

                // Speaker button

                if (is_on_speaker) style::PushFrameStyle(style::FrameSelected);

                if (
                    ImGui::Button(
                        std::string("SP##").append(el.callsign).c_str(),
                        radio_button
                    ) && freq_active
                ) mClient_->SetOnHeadset(el.freq, is_on_speaker);

                if (is_on_speaker) style::PopFrameStyle();

                ImGui::PopStyleVar(2);
                ImGui::EndGroup();
                ImGui::SameLine(0.f, spacing.x / 2.f);
            }

            if (radio_cols) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, spacing.y / 2.f));
                ImGui::BeginGroup();

                // TX button

                if (tx_state) tx_active
                    ? style::PushFrameStyle(style::FrameRadio)
                    : style::PushFrameStyle(style::FrameSelected);

                if (
                    ImGui::Button(
                        std::string("TX##").append(el.callsign).c_str(),
                        radio_button
                    ) && shared::session::facility > 0
                ) {
                    if (freq_active) {
                        mClient_->SetTx(el.freq, !tx_state);
                    } else {
                        mClient_->AddFrequency(el.freq, el.callsign);
                        mClient_->SetEnableInputFilters(shared::mInputFilter);
                        mClient_->SetEnableOutputEffects(shared::mOutputEffects);
                        mClient_->UseTransceiversFromStation(el.callsign, el.freq);
                        mClient_->SetTx(el.freq, true);
                        mClient_->SetRx(el.freq, true);
                        mClient_->SetRadiosGain(shared::RadioGain / 100.0F);
                    }
                }
                style::UnroundCorners(0b1011, true);

                if (tx_state) style::PopFrameStyle();

                // XC button

                if (xc_state) style::PushFrameStyle(style::FrameSelected);

                if (
                    ImGui::Button(
                        std::string("XC##").append(el.callsign).c_str(),
                        radio_button
                    ) && shared::session::facility > 0
                ) {
                    if (freq_active) {
                        mClient_->SetXc(el.freq, !xc_state);
                    } else {
                        mClient_->AddFrequency(el.freq, el.callsign);
                        mClient_->SetEnableInputFilters(shared::mInputFilter);
                        mClient_->SetEnableOutputEffects(shared::mOutputEffects);
                        mClient_->UseTransceiversFromStation(el.callsign, el.freq);
                        mClient_->SetTx(el.freq, true);
                        mClient_->SetRx(el.freq, true);
                        mClient_->SetXc(el.freq, true);
                        mClient_->SetRadiosGain(shared::RadioGain / 100.0F);
                    }
                }
                style::UnroundCorners(0b1101, true);

                if (xc_state) style::PopFrameStyle();

                ImGui::PopStyleVar();
                ImGui::EndGroup();
            }
        }

        ImGui::PopStyleVar();
        ImGui::EndTable();

        ImGui::PopStyleVar(2);
        ImGui::EndChild();
    }

    // Modals

    modals::Settings::render(mClient_, [this]() -> void { playErrorSound(); });

    {
        ImGui::SetNextWindowSize(ImVec2(300, -1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, spacing);

        if (ImGui::BeginPopupModal(
            "Error", nullptr,
            ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
        )) {
            vector_audio::util::TextCentered(lastErrorModalMessage_);

            ImGui::NewLine();
            if (ImGui::Button("OK", ImVec2(-FLT_MIN, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::PopStyleVar();
    }

    // Modal triggers

    if (show_settings) {
        // Update all available data
        vector_audio::shared::availableAudioAPI = mClient_->GetAudioApis();
        vector_audio::shared::availableInputDevices
            = mClient_->GetAudioInputDevices(vector_audio::shared::mAudioApi);
        vector_audio::shared::availableOutputDevices
            = mClient_->GetAudioOutputDevices(vector_audio::shared::mAudioApi);
        ImGui::OpenPopup("Settings Panel");
    }

    if (showErrorModal_) {
        ImGui::OpenPopup("Error");
        showErrorModal_ = false;
    }

    // Clear out the old API data every 500ms
    auto current_time = std::chrono::high_resolution_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - shared::currentlyTransmittingApiTimer)
            .count()
        >= 300) {
        const std::lock_guard<std::mutex> lock(
            vector_audio::shared::transmitting_mutex);
        shared::currentlyTransmittingApiData = "";

        shared::currentlyTransmittingApiData.append(
            live_received_callsigns.empty()
                ? ""
                : std::accumulate(++live_received_callsigns.begin(),
                    live_received_callsigns.end(),
                    *live_received_callsigns.begin(),
                    [](auto& a, auto& b) { return a + "," + b; }));
        shared::currentlyTransmittingApiTimer = current_time;
    }

    ImGui::PopStyleVar();
    ImGui::End();
}

void App::errorModal(std::string message)
{
    this->showErrorModal_ = true;
    lastErrorModalMessage_ = std::move(message);
}

bool App::frequencyExists(int freq)
{
    return std::find_if(shared::FetchedStations.begin(),
               shared::FetchedStations.end(),
               [&freq](const auto& obj) { return obj.freq == freq; })
        != shared::FetchedStations.end();
}
void App::disconnectAndCleanup()
{
    if (!mClient_) {
        return;
    }

    mClient_->Disconnect();
    mClient_->StopAudio();

    for (const auto& f : shared::FetchedStations)
        mClient_->RemoveFrequency(f.freq);

    shared::FetchedStations.clear();
    shared::bootUpVccs = false;
}

} // namespace vector_audio::application