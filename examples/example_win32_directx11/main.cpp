#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <string>
#include <cstring>
#include <vector>
#include <cmath>
#include <../examples/example_win32_directx11/Fonts.h>
#include <../misc/freetype/imgui_freetype.h>
#include <../imgui_internal.h>
#include <../examples/example_win32_directx11/SHA256/sha256.h>
#include <../examples/example_win32_directx11/XOR/xorstr.hpp>
#include <iostream>
#include <fstream>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <ShlObj.h>
#include <wchar.h>
#include <chrono>
#include <future>
#include <cctype>
using json = nlohmann::json;

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;


std::string g_handshakeTokenForActivation;

static bool g_isAnimating = false;
static float g_animationTime = 0.0f;
bool is_license_valid = true;
#define IM_PI 3.14159265358979323846f
static char g_activationKey[51] = "";

ImFont* g_pFont = nullptr;


bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

enum class LoaderState {
    INITIAL_CHECK,
    LOADING,
    ACTIVATION,
    MAIN_LOADER,
    ERROR_STATE,
};

enum CheckState {
    LOADING_MODULES,
    CHECKING_UPDATE,
    UPDATING_MODULES,
    NO_UPDATES_FOUND,
    PERFORMING_SYNC
};

struct AnimationInfo {
    bool isAnimating = false;
    bool isFadingOut = false;
    bool isFadingIn = false;
    float alpha = 1.0f;
};

struct VersionCheckResult {
    bool request_ok = false;
    bool update_required = false;
    std::string handshake_token;
    std::string original_intent;
};


static LoaderState g_loaderState = LoaderState::INITIAL_CHECK;
static LoaderState g_nextLoaderState = LoaderState::INITIAL_CHECK;
static CheckState g_checkState = LOADING_MODULES;
std::string g_errorMessage;

struct SubscriptionInfo {
    std::string name;
    std::string version;
    std::string patch_note;
    int days_remaining;
};

struct ActivationResult {
    bool success = false;
    std::vector<SubscriptionInfo> subscriptions;
    std::string server_error_message;
};

std::vector <SubscriptionInfo> g_userSubscriptions;

void ConfigureSecureSession(cpr::Session& session) {
    session.SetUserAgent(cpr::UserAgent{ xorstr_("MyLoader") });
    session.SetTimeout(cpr::Timeout{ 15000 });

    session.SetOption(cpr::Ssl(cpr::ssl::TLSv1_2{}));
}

std::wstring getExecutablePath() {
    const DWORD maxPath = 300;
    wchar_t exePath[maxPath];
    DWORD length = GetModuleFileNameW(NULL, exePath, maxPath);
    return std::wstring(exePath, length);
}

std::string CalculateChecksum() {
    std::wstring exePath = getExecutablePath(); // Get the executable path

    if (exePath.empty()) {
        g_loaderState = LoaderState::ERROR_STATE; // Assuming you add ERROR_STATE enum
        g_errorMessage = "Could not locate application path.";
        return "";
    }

    std::ifstream file(exePath.c_str(), std::ios::binary); // Open the executable in binary mode
    if (file) {
        std::vector<char> buffer(std::istreambuf_iterator<char>(file), {});

        file.close();

        SHA256 sha256;

        std::string executableHash = sha256(buffer.data(), buffer.size());

        return executableHash;
    }
    g_errorMessage = "Failed to open application file for verification.";
    return "";
}


std::wstring GetSessionFilePath() {
    wchar_t* locateFolder = NULL;
    HRESULT path = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, NULL, &locateFolder);

    if (SUCCEEDED(path)) {
        std::wstring fullPath(locateFolder);

        CoTaskMemFree(locateFolder);

        fullPath += xorstr_(L"\\Loader");

        CreateDirectoryW(fullPath.c_str(), NULL);

        fullPath += xorstr_(L"\\session.dat");

        return fullPath;
    }

    return L"";
}


bool DoesSessionFileExist() {
    std::wstring filePath = GetSessionFilePath();
    if (filePath.empty()) {
        return false; // The helper failed to get the path.
    }
    std::ifstream sessionFile(filePath.c_str());
    return sessionFile.is_open();
}


bool CreateSessionFile(const std::string& token) {
    std::wstring filePath = GetSessionFilePath();
    if (filePath.empty()) {
        return false;
    }

    std::ofstream sessionFile(filePath.c_str(), std::ios::out | std::ios::trunc);
    if (sessionFile.is_open()) {
        sessionFile << token;
        sessionFile.close();
        return true;
    }
    return false;
}

VersionCheckResult checkIntegrity(const std::string& version, const std::string& checksum, const std::string& intent) { // checksum will be g_executableHash

    try {

        const std::string baseUrl = xorstr_("https://server-api-xe36.onrender.com/api/v1/public/version-check?");
        const std::string versionParam = xorstr_("version=");
        const std::string checksumParam = xorstr_("&checksum=");
        const std::string intentParam = xorstr_("&cintent=");

        std::string finalUrl = xorstr_("https://server-api-xe36.onrender.com/api/v1/public/version-check?version=1.0.0&checksum=123&intent=sync");

        //std::string finalUrl = baseUrl + versionParam + version + checksumParam + checksum + intentParam + intent;
        cpr::Session session;
        ConfigureSecureSession(session);
        session.SetUrl(cpr::Url{ finalUrl });
        cpr::Response res = session.Get();

        //auto res = cpr::Get(cpr::Url{ finalUrl });

        if (res.status_code == 200) {

            VersionCheckResult result;
            result.request_ok = true;
            json response = json::parse(res.text);

            // Check the application-level status from the server
            if (response.value("status", "") == "update_required") {
                result.update_required = true;
            }

            result.handshake_token = response.value("handshake_token", "");
            result.original_intent = intent;
            return result;
        }

    }
    catch (const std::exception& e) {
        g_errorMessage = "Could not connect to the server.";
        std::cerr << "Error during integrity check: " << e.what() << std::endl;
    }

    return {};
}



bool SyncPreviousUser(const std::string& handshakeToken) {
    std::wstring programDataPath = GetSessionFilePath();
    if (programDataPath.empty()) {
        return false;
    }

    std::ifstream sessionFile(programDataPath.c_str());

    if (sessionFile) {
        std::stringstream buffer;
        buffer << sessionFile.rdbuf();
        sessionFile.close();
        std::string sessionToken = buffer.str();

        sessionToken.erase(std::remove_if(sessionToken.begin(), sessionToken.end(), ::isspace), sessionToken.end());

        std::string hwid = xorstr_("5344151340604444"); // Placeholder
        json request_body;
        request_body[xorstr_("hwid")] = hwid;
        request_body[xorstr_("handshake_token")] = handshakeToken;

        cpr::Header headers{ {xorstr_("Authorization"), "Bearer " + sessionToken}, {xorstr_("Content-Type"), "application/json"} };

        cpr::Session session;
        ConfigureSecureSession(session);

        session.SetUrl(cpr::Url{ xorstr_("https://server-api-xe36.onrender.com/api/v1/secure/sync") });
        session.SetHeader(headers);
        session.SetBody(cpr::Body{ request_body.dump() });

        g_userSubscriptions.clear();
        cpr::Response res = session.Post();

        if (res.status_code == 200) {
            try {
                json response_data = json::parse(res.text);
                if (response_data.is_array()) {
                    for (const auto& product : response_data) {
                        SubscriptionInfo subInfo;
                        subInfo.name = product["product_name"];
                        subInfo.version = product["latest_version"];
                        subInfo.patch_note = product["patch_note"];
                        subInfo.days_remaining = product["days_remaining"];
                        g_userSubscriptions.push_back(subInfo);
                        std::cout << "Subscriptions loaded successfully." << std::endl;
                    }
                    return true;
                }
                return true; // Sync still succeeded
            }
            catch (const json::exception& e) {
                // _wremove(programDataPath.c_str()); // DEBUG
                std::cout << "JSON error: " << e.what() << std::endl;
                return false;
            }
        }
        else {
            // _wremove(programDataPath.c_str()); // DEBUG
            return false;
        }
    }
    return false;
}


void setImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    auto& colors = style.Colors;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(22 / 255.0f, 22 / 255.0f, 24 / 255.0f, 1);
    colors[ImGuiCol_Border] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    style.WindowRounding = 6.0f;
}

AnimationInfo HandleCrossFadeAnimation(bool& isAnimatingFlag, float& timer, float totalDuration)
{
    AnimationInfo result; // Start with default values (not animating, alpha 1.0)

    if (!isAnimatingFlag) {
        return result; // Not animating, so we're done.
    }

    // If we get here, an animation is active.
    result.isAnimating = true;
    timer += ImGui::GetIO().DeltaTime;

    const float fade_duration = totalDuration / 2.0f;

    // Phase 1: Fade Out
    if (timer < fade_duration) {
        result.isFadingOut = true;
        result.alpha = ImLerp(1.0f, 0.0f, timer / fade_duration);
    }
    // Phase 2: Fade In
    else if (timer < totalDuration) {
        result.isFadingIn = true;
        result.alpha = ImLerp(0.0f, 1.0f, (timer - fade_duration) / fade_duration);
    }
    // Phase 3: Cleanup
    else {
        isAnimatingFlag = false; // Turn off the animation
        timer = 0.0f;
        result.alpha = 1.0f; // Ensure we end up fully visible
        result.isAnimating = false;
    }

    return result;
}
void RenderCenteredText(const char* text)
{
    ImVec2 window_size = ImGui::GetWindowSize();
    float localCenterX = window_size.x * 0.5f;
    float localCenterY = window_size.y * 0.5f;

    ImGui::PushFont(g_pFont, 22.0f);

    ImVec2 textSize = ImGui::CalcTextSize(text);

    ImGui::SetCursorPos(ImVec2(localCenterX - textSize.x * 0.5f, localCenterY + 45.0f));

    ImGui::TextUnformatted(text);

    ImGui::PopFont();
}
const char* GetTextForSubState(CheckState state) {
    switch (state) {
    case LOADING_MODULES: return "Loading modules";
    case CHECKING_UPDATE: return "Checking for updates";
    case UPDATING_MODULES: return "Updating software";
    case NO_UPDATES_FOUND: return "No updates found";
    case PERFORMING_SYNC:  return "Authenticating...";
    default: return "";
    }
}

void RenderLineLoadingAnimation(float deltaTime) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();

    static float lineProgress = 0.0f;

    const ImVec2 rectSize(200.0f, 10.0f);
    const float shadowOffset = 5.0f;
    const float lineHeight = 10.0f;
    const float lineWidth = 25.0f;
    const float lineSpeed = 150.0f;

    float centerX = window_pos.x + window_size.x * 0.5f;
    float centerY = window_pos.y + window_size.y * 0.5f;

    ImVec2 rectPosition(centerX - rectSize.x * 0.5f, centerY - rectSize.y * 0.5f);

    drawList->AddRectFilled(ImVec2(rectPosition.x + shadowOffset, rectPosition.y + shadowOffset),
        ImVec2(rectPosition.x + rectSize.x + shadowOffset, rectPosition.y + rectSize.y + shadowOffset),
        IM_COL32(0, 0, 0, 80), 10.0f);

    drawList->AddRectFilled(rectPosition,
        ImVec2(rectPosition.x + rectSize.x, rectPosition.y + rectSize.y),
        IM_COL32(169, 169, 169, 100), 10.0f);


    lineProgress += deltaTime * lineSpeed;
    if (lineProgress > rectSize.x) {
        lineProgress = -lineWidth;
    }

    float lineXStart = rectPosition.x + lineProgress;
    float lineXEnd = lineXStart + lineWidth;

    float visibleLineXStart = max(lineXStart, rectPosition.x);
    float visibleLineXEnd = min(lineXEnd, rectPosition.x + rectSize.x);

    if (lineXStart > rectPosition.x + rectSize.x) {
        lineProgress = -lineWidth;
    }

    if (visibleLineXEnd > visibleLineXStart) {
        drawList->AddRectFilled(ImVec2(visibleLineXStart, rectPosition.y),
            ImVec2(visibleLineXEnd, rectPosition.y + lineHeight),
            IM_COL32(255, 255, 255, 255), 10.0f);
    }

    float localCenterX = window_size.x * 0.5f;
    float localCenterY = window_size.y * 0.5f;

    ImGui::PushFont(g_pFont, 24.0f);
    const char* text1 = "Initializing...";
    ImVec2 text1Size = ImGui::CalcTextSize(text1);
    ImGui::SetCursorPos(ImVec2(localCenterX - text1Size.x * 0.5f, localCenterY - 50.0f));
    ImGui::Text("Initializing...");
    ImGui::PopFont();

    ImGui::PushFont(g_pFont, 18.0f);
    const char* text2 = "Please wait while we prepare the application for you.";
    ImVec2 text2Size = ImGui::CalcTextSize(text2);
    ImGui::SetCursorPos(ImVec2(localCenterX - text2Size.x * 0.5f, localCenterY + 40.0f));
    ImGui::Text("Please wait while we prepare the application for you.");
    ImGui::PopFont();
}

bool DrawStyledButton(const char* label, ImFont* font = nullptr, const ImVec2& size = ImVec2(0, 0)) {
    ImVec4 color_bg = ImVec4(30 / 255.0f, 30 / 255.0f, 32 / 255.0f, 1.0f);
    ImVec4 color_hover = ImVec4(50 / 255.0f, 55 / 255.0f, 60 / 255.0f, 1.0f);
    ImVec4 color_active = color_hover;
    ImVec4 color_text = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    float rounding = 5.0f;
    float border_size = 0.0f;
    ImVec2 frame_padding = ImVec2(10.0f, 5.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, border_size);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, frame_padding);

    ImGui::PushStyleColor(ImGuiCol_Button, color_bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color_active);
    ImGui::PushStyleColor(ImGuiCol_Text, color_text);

    if (font) {
        ImGui::PushFont(font, 20.0f);
    }

    bool clicked = ImGui::Button(label, size);

    if (font) {
        ImGui::PopFont();
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);

    return clicked;
}

int FilterAlphanumeric(ImGuiInputTextCallbackData* data) {
    if (isalnum(data->EventChar)) {
        return 0;
    }
    return 1;
}

bool DrawStyledInputText(const char* label_text, const char* input_id, char* buffer, size_t buffer_size, float widget_width, ImFont* font = nullptr, ImGuiInputTextFlags flags = 0) {

    ImVec4 color_bg = ImVec4(40 / 255.0f, 40 / 255.0f, 42 / 255.0f, 1.0f);
    ImVec4 color_hover = ImVec4(50 / 255.0f, 55 / 255.0f, 60 / 255.0f, 1.0f);
    ImVec4 color_text_input = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 color_text_label = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    float rounding = 5.0f;
    ImVec2 frame_padding = ImVec2(10.0f, 5.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, frame_padding);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, color_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, color_hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, color_hover);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    if (font) ImGui::PushFont(font, 20.0f);


    ImVec2 window_size = ImGui::GetWindowSize();
    float localCenterX = window_size.x * 0.5f;
    float localCenterY = window_size.y * 0.5f;


    ImGui::PushStyleColor(ImGuiCol_Text, color_text_label);
    ImVec2 labelSize = ImGui::CalcTextSize(label_text);

    ImGui::SetCursorPos(ImVec2(localCenterX - labelSize.x * 0.5f, localCenterY - 70.0f));
    ImGui::TextUnformatted(label_text);
    ImGui::PopStyleColor();


    ImGui::PushStyleColor(ImGuiCol_Text, color_text_input);

    ImGui::SetCursorPosX((window_size.x - widget_width) * 0.5f - 44.0f);
    ImGui::SetCursorPosY(localCenterY - 15.0f);
    ImGui::SetNextItemWidth(widget_width);
    ImGui::InputText(input_id, buffer, buffer_size, flags | ImGuiInputTextFlags_CallbackCharFilter, FilterAlphanumeric);
    ImGui::PopStyleColor();

    ImGui::SameLine();

    bool isDisabled = (strlen(buffer) != 50);
    if (isDisabled) {
        ImGui::BeginDisabled();
    }
    bool buttonClicked = DrawStyledButton("Activate", g_pFont, ImVec2(80.0f, 30.0f));

    if (isDisabled) {
        ImGui::EndDisabled();
    }

    if (font) ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    return buttonClicked && !isDisabled;
}

void RenderCenteredSpinner(float deltaTime, const char* text) {
    const float radius = 18.2f;
    const float lineWidth = 6.0f;
    const float speed = 9.7f;
    const ImU32 color = IM_COL32(121, 118, 201, 255); //  IM_COL32(155, 34, 32, 255);

    ImVec2 contentStart = ImGui::GetCursorScreenPos();

    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    ImVec2 center(contentStart.x + contentSize.x * 0.5f, contentStart.y + contentSize.y * 0.5f);


    static float rotation = 0.0f;
    rotation = fmodf(rotation + (speed * deltaTime), 2.0f * IM_PI);


    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float start_angle = rotation;
    const float end_angle = rotation + IM_PI; // A half-circle arc
    const int num_segments = 64; // Increased for better smoothness

    drawList->PathArcTo(center, radius, start_angle, end_angle, num_segments);
    drawList->PathStroke(color, false, lineWidth);

    ImVec2 window_size = ImGui::GetWindowSize();

    float localCenterX = window_size.x * 0.5f;
    float localCenterY = window_size.y * 0.5f;


    ImGui::PushFont(g_pFont, 22.0f);
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2(localCenterX - textSize.x * 0.5f, localCenterY + 45.0f));
    ImGui::Text(text);
    ImGui::PopFont();
}

void RenderInitialCheckContent()
{
    static bool isTextAnimating = false;
    static float textAnimTimer = 0.0f;
    static CheckState current_sub_state = LOADING_MODULES;
    static CheckState next_sub_state;

    static bool startup_sequence_complete = false;

    static std::future<std::string> checksum_future;
    static std::future<VersionCheckResult> integrity_future;
    static std::future<bool> sync_future;

    static std::string calculated_checksum = "";
    static std::string received_handshake = "";
    static VersionCheckResult result;

    static LoaderState last_parent_state = g_loaderState;
    if (g_loaderState != last_parent_state) {
        current_sub_state = LOADING_MODULES;
        isTextAnimating = false;
        textAnimTimer = 0.0f;
        last_parent_state = g_loaderState;
    }


    RenderCenteredSpinner(ImGui::GetIO().DeltaTime, "");

    bool wasTextAnimating = isTextAnimating;
    AnimationInfo textAnim = HandleCrossFadeAnimation(isTextAnimating, textAnimTimer, 0.8f);

    if (wasTextAnimating && !isTextAnimating) {
        current_sub_state = next_sub_state;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, textAnim.alpha);
    if (textAnim.isFadingOut) {
        RenderCenteredText(GetTextForSubState(current_sub_state));
    }
    else if (textAnim.isFadingIn) {
        RenderCenteredText(GetTextForSubState(next_sub_state));
    }
    else {
        RenderCenteredText(GetTextForSubState(current_sub_state));
    }
    ImGui::PopStyleVar();

    if (ImGui::GetFrameCount() < 2) return;
    if (startup_sequence_complete) {
        RenderCenteredText(GetTextForSubState(current_sub_state));
        return;
    }

    if (isTextAnimating) return;
    switch (current_sub_state) {
        case LOADING_MODULES: {
            if (!checksum_future.valid()) {
                checksum_future = std::async(std::launch::async, CalculateChecksum);
            }
            if (checksum_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                calculated_checksum = checksum_future.get();
                if (calculated_checksum.empty()) {
                    g_loaderState = LoaderState::ERROR_STATE;
                }
                else {
                    next_sub_state = CHECKING_UPDATE;
                    isTextAnimating = true;
                }
            }
            break;
        }
        case CHECKING_UPDATE: {
            if (!integrity_future.valid()) {
                std::string intent = DoesSessionFileExist() ? "sync" : "activate";
                integrity_future = std::async(std::launch::async, checkIntegrity, xorstr_("1.0.0"), calculated_checksum, "sync");
            }
            if (integrity_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                result = integrity_future.get();
                if (!result.request_ok) {
                    g_loaderState = LoaderState::ERROR_STATE;
                }
                else if (result.update_required) {
                    received_handshake = result.handshake_token;
                    next_sub_state = UPDATING_MODULES;
                    isTextAnimating = true;
                }
                else if (!result.handshake_token.empty()) {
                    received_handshake = result.handshake_token;
                    next_sub_state = NO_UPDATES_FOUND;
                    isTextAnimating = true;
                }
                else {
                    g_errorMessage = "Invalid response from server.";
                    g_loaderState = LoaderState::ERROR_STATE;
                }
            }
            break;
        }
        case UPDATING_MODULES: {
            break;
        }
        case NO_UPDATES_FOUND: {
            // No update needed now we either sync or go to activation
            if (result.original_intent == "sync") {

                if (!sync_future.valid()) {  // Check if we have an ongoing sync operation
                    std::cout << "Syncing previous user with handshake: " << received_handshake << std::endl;
                    sync_future = std::async(std::launch::async, SyncPreviousUser, received_handshake);
                }
                if (sync_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {  // If the worker is done
                    if (sync_future.get() && !g_userSubscriptions.empty()) {  // If we successfully synced and have subscriptions
                        g_nextLoaderState = LoaderState::MAIN_LOADER;
                    }
                    else { // If sync failed or no subscriptions found
                        std::cout << "is g_userSubscriptions empty yes or no: " << g_userSubscriptions.empty() << std::endl;
                        std::cout << "future.get() returned false, indicating sync failed." << std::endl;
                        std::cout << "Sync failed or no subscriptions found, proceeding to activation." << std::endl;
                        g_handshakeTokenForActivation = result.handshake_token;
                        g_nextLoaderState = LoaderState::ACTIVATION;
                    }
                    g_isAnimating = true;
                    startup_sequence_complete = true;
                }
            }
            else if (result.original_intent == "activate") { // If the intent was to activate
                g_handshakeTokenForActivation = result.handshake_token;
                g_isAnimating = true;
                startup_sequence_complete = true;
                g_nextLoaderState = LoaderState::ACTIVATION;
            }
            else { // If we got here something went wrong
                g_nextLoaderState = LoaderState::ACTIVATION;
            }
            break;
        }
    }
}



ActivationResult PerformActivation(const std::string& key, const std::string& hwid, const std::string& handshakeToken) {

    ActivationResult result;
    try {
        json request_body;
        request_body[xorstr_("license")] = key;
        request_body[xorstr_("hwid")] = hwid;
        request_body[xorstr_("handshake_token")] = handshakeToken;

        cpr::Response res = cpr::Post(
            cpr::Url{ xorstr_("https://server-api-xe36.onrender.com/api/v1/public/activate") },
            cpr::Header{ {xorstr_("Content-Type"), "application/json"} },
            cpr::Body{ request_body.dump() }
        );
        std::cout << res.text << std::endl;
        if (res.status_code == 200) {
            // Success

            json response = json::parse(res.text);

            if (response.value("status", "") == "success") {
                std::string received_token = response.value("token", "");
                // We try to save the token in session.dat
                if (!received_token.empty() && CreateSessionFile(received_token)) {
                    // Session was saved successfully so now parse the subscription data
                    if (response.contains("subscriptions") && response["subscriptions"].is_array()) {
                        for (const auto& product : response["subscriptions"]) {
                            SubscriptionInfo subInfo;
                            subInfo.name = product.value("product_name", "Unknown");
                            subInfo.version = product.value("latest_version", "0.0.0");
                            subInfo.patch_note = product.value("patch_note", "");
                            subInfo.days_remaining = product.value("days_remaining", 0);
                            result.subscriptions.push_back(subInfo);
                        }
                    }
                    result.success = true;
                }
                else {
                    result.success = false;
                    result.server_error_message = "Activation succeeded, but failed to save session.";
                }
            }
            else {
                result.success = false;
                result.server_error_message = response.value("message", "Activation failed.");
            }

        }
        else {
            // Failure
            result.success = false;
            try {
                json error_response = json::parse(res.text);
                result.server_error_message = error_response.value("error", "An unknown error occurred.");
            }
            catch (const json::parse_error&) {
                result.server_error_message = "Received an invalid error response from the server.";
            }
        }
    }
    catch (const std::exception& e) {
        result.success = false;
        result.server_error_message = "A network error occurred during activation.";
        std::cerr << "Exception in PerformActivation: " << e.what() << std::endl;
    }

    return result;
}

void RenderActivationContent() {
    enum class ActivationState { IDLE, ACTIVATING, FAILED };
    static ActivationState current_state = ActivationState::IDLE;
    static std::future<ActivationResult> activation_future;
    static std::string error_message;
    if (g_handshakeTokenForActivation.empty()) {
        g_loaderState = LoaderState::INITIAL_CHECK; // Or show an error message
        return;
    }

    if (DrawStyledInputText("Enter your license key to proceed", "##activation_key", g_activationKey, sizeof(g_activationKey), 200.0f, g_pFont, ImGuiInputTextFlags_CharsNoBlank)) {
        if (current_state == ActivationState::IDLE) {
            current_state = ActivationState::ACTIVATING;
            std::string hwid = xorstr_("5344151340604444"); // Placeholder

            std::string token_to_use = g_handshakeTokenForActivation;
            g_handshakeTokenForActivation.clear(); // Clear the global token
            activation_future = std::async(std::launch::async, PerformActivation, std::string(g_activationKey), hwid, token_to_use);
        }
    }

    if (current_state == ActivationState::ACTIVATING) {
        RenderCenteredSpinner(ImGui::GetIO().DeltaTime, "");

        if (activation_future.valid() && activation_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            ActivationResult result = activation_future.get();
            if (result.success) {
                g_userSubscriptions = result.subscriptions;
                g_nextLoaderState = LoaderState::MAIN_LOADER;
                g_isAnimating = true;
                current_state = ActivationState::IDLE;
            }
            else {
                error_message = result.server_error_message;
                current_state = ActivationState::FAILED;
            }
        }
    }

    if (current_state == ActivationState::FAILED) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), error_message.c_str());

        static float error_timer = 0.0f;
        error_timer += ImGui::GetIO().DeltaTime;
        if (error_timer > 3.0f) {
            current_state = ActivationState::IDLE;
            error_timer = 0.0f;
        }
    }
}

void RenderMainLoaderContent()
{
    ImGui::Text("Loading main application...");
}

void RenderApplicationUI()
{
    setImGuiStyle();
    static bool first_run = true;
    if (first_run) {
        ImVec2 viewport_center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(viewport_center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        first_run = false;
    }

    ImGui::SetNextWindowSize(ImVec2(300, 200));
    if (ImGui::Begin("Loader", nullptr, ImGuiWindowFlags_NoDecoration))
    {
        bool wasAnimating = g_isAnimating;

        AnimationInfo anim = HandleCrossFadeAnimation(g_isAnimating, g_animationTime, 1.2f);

        if (anim.isAnimating)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim.alpha);

            if (anim.isFadingOut) {
                switch (g_loaderState) {
                case LoaderState::INITIAL_CHECK:
                    RenderInitialCheckContent();
                    break;
                case LoaderState::ACTIVATION:
                    RenderActivationContent();
                    break;
                case LoaderState::MAIN_LOADER:
                    RenderMainLoaderContent();
                    break;
                }
            }
            else if (anim.isFadingIn) {
                switch (g_nextLoaderState) {
                case LoaderState::ACTIVATION:
                    RenderActivationContent();
                    break;
                case LoaderState::MAIN_LOADER:
                    RenderMainLoaderContent();
                    break;
                }
            }
            ImGui::PopStyleVar(1);
        }
        else {
            if (wasAnimating && !g_isAnimating) {
                g_loaderState = g_nextLoaderState;
            }

            switch (g_loaderState)
            {
            case LoaderState::INITIAL_CHECK:
                RenderInitialCheckContent();
                break;
            case LoaderState::ACTIVATION:
                RenderActivationContent();
                break;
            case LoaderState::MAIN_LOADER:
                RenderMainLoaderContent();
                break;
            }
        }
    }
    ImGui::End();
}

bool InitializeModules() {
    g_loaderState = LoaderState::INITIAL_CHECK;
    return true;
}


int main(int, char**)
{
    // Make process DPI aware and obtain main monitor scale
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dear ImGui DirectX11 Example", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1280 * main_scale), (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls



    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale;        // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    //ImGuiFreeType::BuildFontAtlas(io.Fonts);
    ImFontConfig fontConfig;
    fontConfig.FontDataOwnedByAtlas = false;
    fontConfig.FontLoaderFlags = ImGuiFreeTypeBuilderFlags_ForceAutoHint | ImGuiFreeTypeBuilderFlags_LightHinting;
    g_pFont = io.Fonts->AddFontFromMemoryTTF(font, sizeof(font), 24.0f, &fontConfig);

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    InitializeModules();

    // Main loop
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window being minimized or screen locked
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderApplicationUI();

        // Rendering
        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}



bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
