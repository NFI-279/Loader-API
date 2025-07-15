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
#include <iostream>
#include <fstream>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;


std::string g_activationSecret;
std::string g_executableHash;

static bool g_isAnimating = false;
static float g_animationTime = 0.0f;
bool is_license_valid = true;
#define IM_PI 3.14159265358979323846f
static char g_activationKey[13] = "";
static bool g_hasActivationBeenAttempted = false;

ImFont* g_pFont = nullptr;


// Forward declarations of helper functions
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
};

enum CheckState {
    LOADING_MODULES,
    CHECKING_UPDATE,
    UPDATING_MODULES,
    NO_UPDATES_FOUND,
};

struct AnimationInfo {
    bool isAnimating = false;
    bool isFadingOut = false;
    bool isFadingIn = false;
    float alpha = 1.0f; 
};

static LoaderState g_loaderState = LoaderState::INITIAL_CHECK;
static LoaderState g_nextLoaderState = LoaderState::INITIAL_CHECK;
static CheckState g_checkState = LOADING_MODULES;
const std::string g_loaderVersion = "version=1.0.0&checksum=";
std::string g_versionurl = "http://localhost:4000/api/v1/public/version-check?";

std::wstring getExecutablePath() {
    const DWORD maxPath = 300;  
    wchar_t exePath[maxPath];
    GetModuleFileNameW(NULL, exePath, maxPath);
    return std::wstring(exePath, maxPath);
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
    // Get the window size to calculate the center.
    ImVec2 window_size = ImGui::GetWindowSize();
    float localCenterX = window_size.x * 0.5f;
    float localCenterY = window_size.y * 0.5f;

    // Push the font you want to use.
    ImGui::PushFont(g_pFont, 22.0f);

    // Calculate the size of the text to correctly offset it for centering.
    ImVec2 textSize = ImGui::CalcTextSize(text);

    // Set the cursor position before drawing.
    ImGui::SetCursorPos(ImVec2(localCenterX - textSize.x * 0.5f, localCenterY + 45.0f));

    // Draw the text. Using TextUnformatted is slightly faster if you don't have '%' characters.
    ImGui::TextUnformatted(text);

    // Don't forget to pop the font.
    ImGui::PopFont();
}
const char* GetTextForSubState(CheckState state) {
    switch (state) {
    case LOADING_MODULES: return "Loading modules";
    case CHECKING_UPDATE: return "Checking for updates";
    case UPDATING_MODULES: return "Updating software";
    case NO_UPDATES_FOUND: return "No updates found";
    default: return "";
    }
}

void checkIntegrity(std::string url) {
    try {
        std::cout << url;
        auto res = cpr::Get(cpr::Url{ url });
            //cpr::Parameters{ {"abs", version_str}, {"checksum", "123"}});
        json integrityData;
        // Print the get request we make
        std::cout << "Checking integrity..." << std::endl;
        switch(res.status_code) {
            case 200: {
                std::cout << "Integrity check successful." << std::endl;
                integrityData = json::parse(res.text);
                g_activationSecret = integrityData["activation_secret"];
                break;
            }
            case 400: {
                integrityData = json::parse(res.text);
                std::cout << "Error: " << integrityData["error"] << std::endl;
                break;
            }
            case 403: {
                integrityData = json::parse(res.text);
                std::cout << "Error: " << integrityData["error"] << std::endl;
                break;
            }
            case 404: {
                integrityData = json::parse(res.text);
                std::cout << "Error: " << integrityData["error"] << std::endl;
                break;
            }   
            case 409: {
                integrityData = json::parse(res.text);
                std::cout << "Error: " << integrityData["error"] << std::endl;
                break;
            }
            default: {
                std::cerr << "Integrity check failed with status code: " << res.status_code << std::endl;
                break;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error during integrity check: " << e.what() << std::endl;
        return;
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

    bool isDisabled = (strlen(buffer) != 12);
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
    //const char* text = "Checking for updates";
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

    if (!isTextAnimating) {
        switch (current_sub_state) {
        case LOADING_MODULES:
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                next_sub_state = CHECKING_UPDATE;
                isTextAnimating = true;
                
            }
            break;
        case CHECKING_UPDATE:
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                next_sub_state = UPDATING_MODULES;
                isTextAnimating = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
                next_sub_state = NO_UPDATES_FOUND;
                isTextAnimating = true;
            }
            break;
        case UPDATING_MODULES:
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                g_nextLoaderState = LoaderState::ACTIVATION;
                g_isAnimating = true;
            }
            break;
        case NO_UPDATES_FOUND:
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                g_nextLoaderState = LoaderState::ACTIVATION;
                g_isAnimating = true;
            }
            break;
        }
    }
}

void RenderActivationContent() {
    if (DrawStyledInputText("Enter your license key to proceed", "##activation_key", g_activationKey, sizeof(g_activationKey), 200.0f, g_pFont, ImGuiInputTextFlags_CharsNoBlank))
    {
        g_hasActivationBeenAttempted = true; // Record that a check was performed

        if (is_license_valid) {
            g_nextLoaderState = LoaderState::MAIN_LOADER;
            g_isAnimating = true;
        }
    }
    if (g_hasActivationBeenAttempted)
    {
        bool is_key_still_invalid = (strlen(g_activationKey) != 12); // Check condition again
        if (is_key_still_invalid) {
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Error: Invalid license key.");
        }
    }
}

void RenderMainLoaderContent()
{
    ImGui::Text("Loading main application...");
}

bool InitializeModules() {
    std::wstring exePath = getExecutablePath(); // Get the executable path
    std::ifstream file;
    
    file.open(exePath.c_str(), std::ios::binary); // Open the executable in binary mode
    if (file) {
        std::vector<char> buffer;

        file.seekg(0, std::ios::end); // Go to the end of the file
        size_t fileSize = file.tellg(); // Read that position to get the file size

        file.seekg(0, std::ios::beg); // Go back to the beginning of the file

        buffer.resize(fileSize); // Resize the buffer to the file size

        file.read(buffer.data(), fileSize); // Read the entire file into the buffer

        file.close();

        SHA256 sha256;
        g_executableHash = sha256(buffer.data(), fileSize); // Calculate the SHA256 hash of the file
        std::cout << g_executableHash << std::endl; // Print the SHA256 hash of the file
        g_versionurl = g_versionurl.append(g_loaderVersion).append(g_executableHash);
        checkIntegrity(g_versionurl);
        return true;
    }
    else {
        std::cout << "Failed to open the executable file." << std::endl;
        return false;
    }
}

void RenderApplicationUI()
{
    ImGui::ShowMetricsWindow();
    setImGuiStyle();
    static bool first_run = true;
    if (first_run) {
        ImVec2 viewport_center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(viewport_center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        first_run = false;
    }
    bool is_key_length_valid = (strlen(g_activationKey) == 12);

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
        }else {
            if (wasAnimating) {
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

// Main code
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

    if (!InitializeModules()) {
        std::cerr << "Failed to initialize modules." << std::endl;
        // close the application or reload it
    }

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
        HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
        //HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
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

// Helper functions

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

// Forward declare message handler from imgui_impl_win32.cpp
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
