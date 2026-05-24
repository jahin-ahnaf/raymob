#include "raylib.h"
#include "enet.hpp"
#include "scripts/objects.hpp"
#include "raymob.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>

using namespace std;
using namespace gameenet;

namespace {

    constexpr int kDefaultPort = 7777;
    constexpr float kPlayerSize = 0.2f;
    constexpr float kPlayerSpeed = 180.0f;
    constexpr int kPlayerFrames = 5;

    constexpr array<const char*, 4> kColorNames = { "Red", "Blue", "Green", "Pink" };
    constexpr array<Color, 4> kColorPalette = { RED, BLUE, GREEN, PINK };

    enum class ScreenState {
        Loading,
        Menu,
        Settings,
        Connecting,
        Playing,
        Error
    };

    enum class InputField {
        Name,
        RoomCode,
        ServerHost
    };

    struct VirtualJoystick {
        Vector2 center{};
        float radius = 68.0f;
        bool active = false;
        Vector2 value{};

        void Update(const Vector2& newCenter, float newRadius) {
            center = newCenter;
            radius = newRadius;

            const int touchCount = GetTouchPointCount();
            const bool touchDown = touchCount > 0;
            const bool mouseDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
            const bool pointerDown = touchDown || mouseDown;
            Vector2 pointer = touchDown ? GetTouchPosition(0) : GetMousePosition();

            if (!active && pointerDown) {
                const float dx = pointer.x - center.x;
                const float dy = pointer.y - center.y;
                if (dx * dx + dy * dy <= radius * radius * 1.3f) {
                    active = true;
                }
            }

            if (active) {
                if (pointerDown) {
                    Vector2 delta = { pointer.x - center.x, pointer.y - center.y };
                    float len = sqrtf(delta.x * delta.x + delta.y * delta.y);
                    if (len > radius && len > 0.0f) {
                        delta.x = delta.x / len * radius;
                        delta.y = delta.y / len * radius;
                    }
                    value = { delta.x / radius, delta.y / radius };
                } else {
                    active = false;
                    value = {};
                }
            } else {
                value = {};
            }
        }

        void Draw() const {
            const Color base = Fade(BLACK, 0.35f);
            const Color rim = Fade(WHITE, 0.45f);
            const Vector2 knob = { center.x + value.x * radius, center.y + value.y * radius };

            DrawCircleV(center, radius, base);
            DrawCircleLines((int)center.x, (int)center.y, radius, rim);
            DrawCircleV(knob, radius * 0.42f, Fade(SKYBLUE, active ? 0.85f : 0.55f));
        }
    };

    struct VisualPlayer {
        Player sprite;
        bool active = false;
        std::uint32_t clientId = 0;
        std::uint8_t colorIndex = 0;

        void Clear() {
            if (active) {
                sprite.Unload();
            }
            active = false;
            clientId = 0;
            colorIndex = 0;
        }

        void Apply(const PlayerView& view, const array<Texture2D, 4>& playerTextures, const Texture2D& shadowTexture) {
            const bool needsReload = !active || clientId != view.clientId || colorIndex != view.colorIndex;
            if (needsReload) {
                if (active) {
                    sprite.Unload();
                }
                sprite.CreateFromTextures(
                        view.name,
                        playerTextures[view.colorIndex],
                        shadowTexture,
                        view.x,
                        view.y,
                        kPlayerSize,
                        kPlayerSpeed,
                        kPlayerFrames
                );
                colorIndex = view.colorIndex;
                clientId = view.clientId;
                active = true;
            }

            sprite.name = view.name;
            sprite.x = view.x;
            sprite.y = view.y;
            sprite.Animate(view.moving);
            sprite.SetFacing(view.facingLeft);
        }
    };

    struct UiState {
        string name = "Afnan";
        string roomCode = "ABCD1234";
        string serverHost = "127.0.0.1";
        InputField focused = InputField::Name;
        string message = "Enter an 8-character room code to join or create.";
        string error;
    };

    struct SettingsState {
#if defined(__ANDROID__)
        bool touchControls = true;
#else
        bool touchControls = false;
#endif
        bool fullscreen = true;
        int port = kDefaultPort;
    };

    struct ChatState {
        bool open = false;
        string draft;
    };

#if defined(__ANDROID__)
    void SetSoftKeyboardVisible(bool visible) {
        static bool keyboardVisible = false;
        if (keyboardVisible == visible) return;
        keyboardVisible = visible;

        if (visible) {
            ShowSoftKeyboard();
        } else {
            HideSoftKeyboard();
        }
    }
#else
    void SetSoftKeyboardVisible(bool) {}
#endif

    struct GameAssets {
        array<Texture2D, 4> playerTextures{};
        Texture2D shadowTexture{};
        bool loaded = false;
    };

    void DrawMenuBackground();

    string ResolveAssetPath(const string& relative) {
        if (FileExists(relative.c_str())) return relative;

#if defined(__linux__)
        char exePath[4096]{};
    const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        string base(exePath);
        const size_t slash = base.find_last_of('/');
        if (slash != string::npos) {
            base = base.substr(0, slash + 1) + relative;
            if (FileExists(base.c_str())) return base;
        }
    }
#endif

        return relative;
    }

    void DrawLoadingScreen(const string& title, float progress, const string& detail) {
        BeginDrawing();
        DrawMenuBackground();
        DrawText(title.c_str(), 80, 72, 44, RAYWHITE);
        DrawText(detail.c_str(), 80, 128, 20, Fade(RAYWHITE, 0.85f));

        Rectangle bar = { 80, 184, (float)GetScreenWidth() - 160.0f, 28 };
        DrawRectangleRounded(bar, 0.2f, 8, Fade(BLACK, 0.7f));
        DrawRectangleRoundedLines(bar, 0.2f, 8, 2.0f, Fade(WHITE, 0.35f));
        Rectangle fill = bar;
        fill.width = bar.width * std::clamp(progress, 0.0f, 1.0f);
        DrawRectangleRounded(fill, 0.2f, 8, SKYBLUE);
        DrawText(TextFormat("%.0f%%", progress * 100.0f), (int)bar.x + (int)bar.width - 64, (int)bar.y + 2, 20, RAYWHITE);
        EndDrawing();
    }

    void UnloadGameAssets(GameAssets& assets);

    bool LoadGameAssets(GameAssets& assets) {
        const array<string, 5> paths = {
                ResolveAssetPath("resources/animation/red_base_spritesheet.png"),
                ResolveAssetPath("resources/animation/blue_base_spritesheet.png"),
                ResolveAssetPath("resources/animation/green_base_spritesheet.png"),
                ResolveAssetPath("resources/animation/pink_base_spritesheet.png"),
                ResolveAssetPath("resources/sprites/playerShadow.png"),
        };

        const array<string, 5> labels = {
                "Loading red player sprite...",
                "Loading blue player sprite...",
                "Loading green player sprite...",
                "Loading pink player sprite...",
                "Loading shadow sprite...",
        };

        for (size_t i = 0; i < 5; ++i) {
            if (i < 4) {
                assets.playerTextures[i] = LoadTexture(paths[i].c_str());
                if (assets.playerTextures[i].id == 0) {
                    UnloadGameAssets(assets);
                    return false;
                }
            } else {
                assets.shadowTexture = LoadTexture(paths[i].c_str());
                if (assets.shadowTexture.id == 0) {
                    UnloadGameAssets(assets);
                    return false;
                }
            }
            DrawLoadingScreen("Loading assets", (float)(i + 1) / 5.0f, labels[i]);
        }

        assets.loaded = true;
        DrawLoadingScreen("Loading assets", 1.0f, "Sprites ready");
        return true;
    }

    void UnloadGameAssets(GameAssets& assets) {
        for (auto& texture : assets.playerTextures) {
            if (texture.id != 0) UnloadTexture(texture);
            texture = {};
        }
        if (assets.shadowTexture.id != 0) UnloadTexture(assets.shadowTexture);
        assets.shadowTexture = {};
        assets.loaded = false;
    }

    string MakeUpperCode(string code) {
        string result;
        for (char c : code) {
            if (isalnum((unsigned char)c) && result.size() < kRoomCodeLength) {
                result.push_back((char)toupper((unsigned char)c));
            }
        }
        return result;
    }

    string SanitizeName(string name) {
        string out;
        for (char c : name) {
            if ((c >= 32 && c <= 126) && out.size() < kMaxNameLength) {
                out.push_back(c);
            }
        }
        return out;
    }

    string SanitizeHost(string host) {
        string out;
        for (char c : host) {
            if ((isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':') && out.size() < 63) {
                out.push_back(c);
            }
        }
        return out;
    }

    bool IsRoomCodeValid(const string& code) {
        if (code.size() != kRoomCodeLength) return false;
        for (char c : code) {
            if (!isalnum((unsigned char)c)) return false;
        }
        return true;
    }

    void DrawCenteredText(const char* text, Rectangle rect, int fontSize, Color color) {
        const int width = MeasureText(text, fontSize);
        DrawText(text, (int)(rect.x + rect.width * 0.5f - width * 0.5f), (int)(rect.y + rect.height * 0.5f - fontSize * 0.5f), fontSize, color);
    }

    bool Button(Rectangle rect, const char* label) {
        Vector2 mouse = GetMousePosition();
        bool hover = CheckCollisionPointRec(mouse, rect);
        Color fill = hover ? Fade(DARKBLUE, 0.92f) : Fade(BLACK, 0.72f);
        Color border = hover ? SKYBLUE : Fade(WHITE, 0.35f);

        DrawRectangleRounded(rect, 0.2f, 8, fill);
        DrawRectangleRoundedLines(rect, 0.2f, 8, 2.0f, border);
        DrawCenteredText(label, rect, 22, RAYWHITE);
        return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    void DrawInputField(Rectangle rect, const char* label, const string& value, bool focused) {
        DrawText(label, (int)rect.x, (int)rect.y - 24, 18, Fade(RAYWHITE, 0.8f));
        DrawRectangleRounded(rect, 0.15f, 8, focused ? Fade(DARKBLUE, 0.9f) : Fade(BLACK, 0.7f));
        DrawRectangleRoundedLines(rect, 0.15f, 8, 2.0f, focused ? SKYBLUE : Fade(WHITE, 0.3f));
        DrawText(value.c_str(), (int)rect.x + 12, (int)rect.y + 10, 22, RAYWHITE);
    }

    bool HandleTextInput(string& value, bool roomCode, bool hostField) {
        bool enterPressed = false;
#if defined(__ANDROID__)
        char softKey = GetLastSoftKeyChar();
        while (softKey != '\0') {
            char c = softKey;
            bool accept = false;
            if (roomCode) {
                accept = isalnum((unsigned char)c);
                c = (char)toupper((unsigned char)c);
            } else if (hostField) {
                accept = isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':';
            } else {
                accept = c >= 32 && c <= 126;
            }
            if (softKey == '\n') {
                enterPressed = true;
            } else if (softKey == '\b') {
                if (!value.empty()) value.pop_back();
            } else if (accept && value.size() < (roomCode ? kRoomCodeLength : (hostField ? 63u : (std::size_t)kMaxNameLength))) {
                value.push_back(c);
            }
            ClearLastSoftKey();
            softKey = GetLastSoftKeyChar();
        }
#else
        int key = GetCharPressed();
        while (key > 0) {
            char c = (char)key;
            bool accept = false;
            if (c == '\n' || c == '\r') {
                enterPressed = true;
            }
            if (roomCode) {
                accept = isalnum((unsigned char)c);
                c = (char)toupper((unsigned char)c);
            } else if (hostField) {
                accept = isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':';
            } else {
                accept = c >= 32 && c <= 126;
            }
            if (accept && c != '\n' && c != '\r' && value.size() < (roomCode ? kRoomCodeLength : (hostField ? 63u : (std::size_t)kMaxNameLength))) {
                value.push_back(c);
            }
            key = GetCharPressed();
        }
#endif
        return enterPressed;
    }

    Vector2 ReadMovementInput(VirtualJoystick& joystick, bool touchControlsEnabled) {
        if (touchControlsEnabled) {
            const float radius = std::min((float)GetScreenHeight() * 0.11f, 76.0f);
            const Vector2 center = { radius + 32.0f, (float)GetScreenHeight() - radius - 32.0f };
            joystick.Update(center, radius);
        } else {
            joystick.active = false;
            joystick.value = {};
        }

        Vector2 movement = {};
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) movement.x -= 1.0f;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) movement.x += 1.0f;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) movement.y -= 1.0f;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) movement.y += 1.0f;

        movement.x += joystick.value.x;
        movement.y += joystick.value.y;

        movement.x = std::clamp(movement.x, -1.0f, 1.0f);
        movement.y = std::clamp(movement.y, -1.0f, 1.0f);
        return movement;
    }

    Rectangle HudPanelRect() {
        return Rectangle{ 14, 14, 330, 130 };
    }

    void DrawHud(const ClientNet& client, const UiState& ui, const vector<PlayerView>& players) {
        Rectangle panel = HudPanelRect();
        DrawRectangleRounded(panel, 0.18f, 8, Fade(BLACK, 0.7f));
        DrawRectangleRoundedLines(panel, 0.18f, 8, 2.0f, Fade(SKYBLUE, 0.65f));

        int y = (int)panel.y + 10;
        DrawText(("Player: " + ui.name).c_str(), (int)panel.x + 12, y, 18, RAYWHITE); y += 24;
        DrawText(("Room: " + client.joinInfo().roomCode).c_str(), (int)panel.x + 12, y, 18, RAYWHITE); y += 24;
        DrawText(("Color: " + string(kColorNames[min((std::size_t)client.joinInfo().colorIndex, kColorNames.size() - 1)])).c_str(), (int)panel.x + 12, y, 18, RAYWHITE); y += 24;
        DrawText(("FPS: " + to_string(GetFPS()) + "   Ping: " + to_string(client.pingMs()) + " ms").c_str(), (int)panel.x + 12, y, 18, RAYWHITE); y += 24;
        DrawText(("Players: " + to_string(players.size()) + "/4").c_str(), (int)panel.x + 12, y, 18, RAYWHITE);
    }

    void DrawSettingsPanel(SettingsState& settings, bool inGame) {
        Rectangle panel = { 64, 64, (float)GetScreenWidth() - 128.0f, (float)GetScreenHeight() - 128.0f };
        DrawRectangleRounded(panel, 0.12f, 8, Fade(BLACK, 0.82f));
        DrawRectangleRoundedLines(panel, 0.12f, 8, 2.0f, Fade(SKYBLUE, 0.7f));
        DrawText(inGame ? "In-game settings" : "Settings", (int)panel.x + 24, (int)panel.y + 18, 34, RAYWHITE);
        DrawText("Press / in room for chat. F1 opens in-game settings.", (int)panel.x + 24, (int)panel.y + 58, 18, Fade(RAYWHITE, 0.8f));
    }

    void DrawChatOverlay(const vector<ChatLine>& log, const ChatState& chat) {
        const float width = min(420.0f, (float)GetScreenWidth() - 24.0f);
        const float x = 12.0f;
        float y = (float)GetScreenHeight() - 180.0f;
        int start = (int)log.size() - 6;
        if (start < 0) start = 0;

        DrawRectangleRounded({ x, y - 12.0f, width, 168.0f }, 0.14f, 8, Fade(BLACK, 0.5f));
        for (int i = start; i < (int)log.size(); ++i) {
            const auto& line = log[i];
            Color nameColor = kColorPalette[min((std::size_t)line.colorIndex, kColorPalette.size() - 1)];
            DrawText((line.name + ":").c_str(), (int)x + 12, (int)y, 18, nameColor);
            DrawText(line.message.c_str(), (int)x + 98, (int)y, 18, RAYWHITE);
            y += 24.0f;
        }

        if (chat.open) {
            Rectangle input = { 12, (float)GetScreenHeight() - 44.0f, min(520.0f, (float)GetScreenWidth() - 24.0f), 32 };
            DrawRectangleRounded(input, 0.16f, 8, Fade(BLACK, 0.78f));
            DrawRectangleRoundedLines(input, 0.16f, 8, 2.0f, SKYBLUE);
            DrawText((string("/") + chat.draft).c_str(), (int)input.x + 12, (int)input.y + 7, 18, RAYWHITE);
        } else {
            DrawText("Press / to chat", 14, GetScreenHeight() - 26, 16, Fade(RAYWHITE, 0.7f));
        }
    }

    void DrawRosterOverlay(const vector<PlayerView>& players) {
        if (!IsKeyDown(KEY_TAB)) return;

        Rectangle panel = { (float)GetScreenWidth() - 260.0f, 14.0f, 246.0f, 34.0f + (float)players.size() * 30.0f };
        DrawRectangleRounded(panel, 0.14f, 8, Fade(BLACK, 0.7f));
        DrawRectangleRoundedLines(panel, 0.14f, 8, 2.0f, Fade(WHITE, 0.28f));
        DrawText("Players in room", (int)panel.x + 14, (int)panel.y + 10, 20, RAYWHITE);

        for (size_t i = 0; i < players.size(); ++i) {
            const auto& player = players[i];
            Color color = kColorPalette[min((std::size_t)player.colorIndex, kColorPalette.size() - 1)];
            int y = (int)panel.y + 42 + (int)i * 30;
            DrawText(player.name.c_str(), (int)panel.x + 14, y, 18, color);
            DrawText(TextFormat("#%u", player.clientId), (int)panel.x + 150, y, 18, Fade(RAYWHITE, 0.7f));
        }
    }

    void DrawMenuBackground() {
        ClearBackground(Color{ 26, 28, 36, 255 });
        const int w = GetScreenWidth();
        const int h = GetScreenHeight();
        for (int y = 0; y < h; y += 64) {
            DrawLine(0, y, w, y, Fade(WHITE, 0.04f));
        }
        for (int x = 0; x < w; x += 64) {
            DrawLine(x, 0, x, h, Fade(WHITE, 0.04f));
        }
    }

    bool SyncSnapshotPlayers(const vector<PlayerView>& views, array<VisualPlayer, 4>& slots, const GameAssets& assets) {
        array<bool, 4> used{};
        for (const auto& view : views) {
            VisualPlayer* slot = nullptr;
            for (auto& candidate : slots) {
                if (candidate.active && candidate.clientId == view.clientId) {
                    slot = &candidate;
                    break;
                }
            }
            if (!slot) {
                for (auto& candidate : slots) {
                    if (!candidate.active) {
                        slot = &candidate;
                        break;
                    }
                }
            }
            if (slot) {
                slot->Apply(view, assets.playerTextures, assets.shadowTexture);
                for (std::size_t i = 0; i < slots.size(); ++i) {
                    if (&slots[i] == slot) {
                        used[i] = true;
                        break;
                    }
                }
            }
        }

        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (!used[i]) {
                slots[i].Clear();
            }
        }
        return !views.empty();
    }

    const VisualPlayer* FindLocalPlayer(const array<VisualPlayer, 4>& slots, std::uint32_t clientId) {
        for (const auto& slot : slots) {
            if (slot.active && slot.clientId == clientId) {
                return &slot;
            }
        }
        return nullptr;
    }

    Vector2 PlayerCenter(const Player& player) {
        return {
                player.x + fabsf(player.rec.width) * player.size * 0.5f,
                player.y + player.rec.height * player.size * 0.5f
        };
    }

    void RunServer(int argc, char** argv) {
        string bindHost = "0.0.0.0";
        int port = kDefaultPort;
        for (int i = 2; i < argc; ++i) {
            string arg = argv[i];
            if (arg == "--bind" && i + 1 < argc) {
                bindHost = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                port = atoi(argv[++i]);
            }
        }

        ServerApp server;
        string error;
        if (!server.start(bindHost, (uint16_t)port, &error)) {
            cerr << error << '\n';
            return;
        }

        cout << "Server listening on " << bindHost << ':' << port << endl;
        server.run();
    }

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (string(argv[i]) == "--server") {
            RunServer(argc, argv);
            return 0;
        }
    }

    UiState ui{};
    SettingsState settings{};
    ChatState chat{};
    GameAssets assets{};
    ClientNet client;
    vector<PlayerView> snapshotPlayers;
    array<VisualPlayer, 4> slots{};
    VirtualJoystick joystick{};
    ScreenState screen = ScreenState::Menu;
    bool inGameSettings = false;

    InitWindow(1280, 720, "Something Cool");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    settings.fullscreen = IsWindowFullscreen();
    settings.port = kDefaultPort;

    Camera2D camera{};
    camera.rotation = 0.0f;
    camera.zoom = 1.0f;

    screen = ScreenState::Loading;
    if (!LoadGameAssets(assets)) {
        BeginDrawing();
        ClearBackground(BLACK);
        DrawText("Failed to load game assets.", 80, 80, 28, RED);
        DrawText("Check the resources folder and relaunch.", 80, 120, 20, RAYWHITE);
        EndDrawing();
        while (!WindowShouldClose()) {}
        UnloadGameAssets(assets);
        CloseWindow();
        return 1;
    }
    screen = ScreenState::Menu;

#if defined(__ANDROID__)
    ScreenState lastScreen = ScreenState::Loading;
    InputField lastFocusedField = ui.focused;
#endif

    while (!WindowShouldClose()) {
        const Rectangle nameRect = { 80, 210, 360, 48 };
        const Rectangle roomRect = { 80, 300, 360, 48 };
        const Rectangle hostRect = { 80, 390, 360, 48 };
        const Rectangle joinRect = { 80, 470, 172, 54 };
        const Rectangle connectRect = { 268, 470, 172, 54 };
        const Rectangle settingsRect = { 80, 540, 172, 54 };

#if defined(__ANDROID__)
        if (screen != lastScreen) {
            if (screen == ScreenState::Menu) {
                SetSoftKeyboardText(
                    ui.focused == InputField::Name ? ui.name.c_str()
                    : ui.focused == InputField::RoomCode ? ui.roomCode.c_str()
                                                         : ui.serverHost.c_str()
                );
                lastFocusedField = ui.focused;
            } else if (screen == ScreenState::Playing && chat.open) {
                SetSoftKeyboardText(chat.draft.c_str());
            }
            lastScreen = screen;
        }
#endif

        if (screen == ScreenState::Menu) {
            if (IsKeyPressed(KEY_TAB)) {
                ui.focused = (ui.focused == InputField::Name) ? InputField::RoomCode : (ui.focused == InputField::RoomCode ? InputField::ServerHost : InputField::Name);
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                Vector2 mouse = GetMousePosition();
                ui.focused = CheckCollisionPointRec(mouse, nameRect) ? InputField::Name
                                                                     : CheckCollisionPointRec(mouse, roomRect) ? InputField::RoomCode
                                                                                                               : CheckCollisionPointRec(mouse, hostRect) ? InputField::ServerHost
                                                                                                                                                         : ui.focused;
            }

            ui.name = SanitizeName(ui.name);
            ui.roomCode = MakeUpperCode(ui.roomCode);
            ui.serverHost = SanitizeHost(ui.serverHost);

#if defined(__ANDROID__)
            if (ui.focused != lastFocusedField) {
                SetSoftKeyboardText(
                    ui.focused == InputField::Name ? ui.name.c_str()
                    : ui.focused == InputField::RoomCode ? ui.roomCode.c_str()
                                                         : ui.serverHost.c_str()
                );
                lastFocusedField = ui.focused;
            }
#endif

            bool enterPressed = false;
            if (ui.focused == InputField::Name) {
                enterPressed = HandleTextInput(ui.name, false, false);
            } else if (ui.focused == InputField::RoomCode) {
                enterPressed = HandleTextInput(ui.roomCode, true, false);
            } else if (ui.focused == InputField::ServerHost) {
                enterPressed = HandleTextInput(ui.serverHost, false, true);
            }

            SetSoftKeyboardVisible(true);

            if (IsKeyPressed(KEY_BACKSPACE)) {
                string* target = &ui.name;
                if (ui.focused == InputField::RoomCode) target = &ui.roomCode;
                else if (ui.focused == InputField::ServerHost) target = &ui.serverHost;
                if (!target->empty()) target->pop_back();
            }

            if (Button(joinRect, "Join / Create") || IsKeyPressed(KEY_ENTER) || enterPressed) {
                ui.roomCode = MakeUpperCode(ui.roomCode);
                if (!IsRoomCodeValid(ui.roomCode)) {
                    ui.error = "Room code must be 8 alphanumeric characters.";
                    screen = ScreenState::Error;
                } else {
                    string error;
                    if (!client.connect(ui.serverHost, (uint16_t)settings.port, ui.roomCode, ui.name, &error)) {
                        ui.error = error.empty() ? client.lastError() : error;
                        screen = ScreenState::Error;
                    } else {
                        snapshotPlayers.clear();
                        for (auto& slot : slots) slot.Clear();
                        ui.message = "Connecting to room " + ui.roomCode + "...";
                        screen = ScreenState::Connecting;
                    }
                }
            }

            if (Button(connectRect, "Clear")) {
                ui.name = "Afnan";
                ui.roomCode = "ABCD1234";
                ui.serverHost = "127.0.0.1";
                ui.focused = InputField::Name;
#if defined(__ANDROID__)
                SetSoftKeyboardText(ui.name.c_str());
                lastFocusedField = ui.focused;
#endif
            }

            if (Button(settingsRect, "Settings")) {
                screen = ScreenState::Settings;
            }

            BeginDrawing();
            DrawMenuBackground();
            DrawText("Something Cool", 80, 70, 52, RAYWHITE);
            DrawText("ENet rooms, four colors, touch joystick, fullscreen", 80, 128, 22, Fade(RAYWHITE, 0.8f));
            DrawInputField(nameRect, "Player name", ui.name, ui.focused == InputField::Name);
            DrawInputField(roomRect, "Room code (8 chars)", ui.roomCode, ui.focused == InputField::RoomCode);
            DrawInputField(hostRect, "Server address (host[:port])", ui.serverHost, ui.focused == InputField::ServerHost);
            DrawText(ui.message.c_str(), 80, 560, 20, Fade(SKYBLUE, 0.9f));
            if (!ui.error.empty()) DrawText(ui.error.c_str(), 80, 590, 20, RED);
            DrawText("WASD / arrows or joystick to move. Room creates automatically if missing.", 80, 645, 18, Fade(RAYWHITE, 0.7f));
            EndDrawing();
            continue;
        }

        if (screen == ScreenState::Settings) {
            SetSoftKeyboardVisible(false);
            Rectangle fullscreenBtn = { 104, 180, 360, 48 };
            Rectangle touchBtn = { 104, 244, 360, 48 };
            Rectangle portMinus = { 104, 308, 48, 48 };
            Rectangle portPlus = { 416, 308, 48, 48 };
            Rectangle backBtn = { 104, 392, 152, 48 };

            if (IsKeyPressed(KEY_ESCAPE)) {
                screen = ScreenState::Menu;
            }

            BeginDrawing();
            DrawMenuBackground();
            DrawSettingsPanel(settings, false);
            DrawText("Adjust presentation and platform controls before joining.", 104, 132, 18, Fade(RAYWHITE, 0.8f));

            if (Button(fullscreenBtn, settings.fullscreen ? "Fullscreen: ON" : "Fullscreen: OFF")) {
                settings.fullscreen = !settings.fullscreen;
                ToggleFullscreen();
                settings.fullscreen = IsWindowFullscreen();
            }
            if (Button(touchBtn, settings.touchControls ? "Touch controls: ON" : "Touch controls: OFF")) {
                settings.touchControls = !settings.touchControls;
            }

            DrawText("Server port", (int)portMinus.x + 60, (int)portMinus.y + 14, 20, RAYWHITE);
            if (Button(portMinus, "-")) {
                settings.port = max(1, settings.port - 1);
            }
            if (Button(portPlus, "+")) {
                settings.port = min(65535, settings.port + 1);
            }
            DrawRectangleRounded({ 162, 308, 238, 48 }, 0.14f, 8, Fade(BLACK, 0.55f));
            DrawRectangleRoundedLines({ 162, 308, 238, 48 }, 0.14f, 8, 2.0f, Fade(WHITE, 0.3f));
            DrawText(TextFormat("%d", settings.port), 254, 322, 22, SKYBLUE);

            if (Button(backBtn, "Back")) {
                screen = ScreenState::Menu;
            }

            EndDrawing();
            continue;
        }

        if (screen == ScreenState::Connecting) {
            SetSoftKeyboardVisible(false);
            string error;
            if (!client.pump(&error)) {
                if (client.connected()) {
                    ui.message = "Connected.";
                    snapshotPlayers = client.players();
                    SyncSnapshotPlayers(snapshotPlayers, slots, assets);
                    screen = ScreenState::Playing;
                } else if (!client.lastError().empty()) {
                    ui.error = error.empty() ? client.lastError() : error;
                    screen = ScreenState::Error;
                }
            } else if (client.connected()) {
                ui.message = "Connected.";
                snapshotPlayers = client.players();
                SyncSnapshotPlayers(snapshotPlayers, slots, assets);
                screen = ScreenState::Playing;
            }

            BeginDrawing();
            ClearBackground(BLACK);
            DrawText("Connecting...", 80, 80, 48, RAYWHITE);
            DrawText(ui.message.c_str(), 80, 140, 22, Fade(RAYWHITE, 0.85f));
            if (!client.lastError().empty()) DrawText(client.lastError().c_str(), 80, 180, 20, RED);
            EndDrawing();
            continue;
        }

        if (screen == ScreenState::Error) {
            SetSoftKeyboardVisible(false);
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                ui.error.clear();
                ui.message = "Enter an 8-character room code to join or create.";
                screen = ScreenState::Menu;
                client.disconnect();
            }
            BeginDrawing();
            DrawMenuBackground();
            DrawText("Connection error", 80, 80, 48, RAYWHITE);
            DrawText(ui.error.c_str(), 80, 150, 22, RED);
            DrawText("Press Enter or Esc to return.", 80, 200, 20, Fade(RAYWHITE, 0.85f));
            EndDrawing();
            continue;
        }

        string error;
        if (IsKeyPressed(KEY_ESCAPE)) {
            SetSoftKeyboardVisible(false);
            chat.open = false;
            chat.draft.clear();
            inGameSettings = false;
            client.disconnect();
            snapshotPlayers.clear();
            for (auto& slot : slots) slot.Clear();
            ui.error.clear();
            ui.message = "Enter an 8-character room code to join or create.";
            screen = ScreenState::Menu;
            continue;
        }

        client.pump(&error);
        snapshotPlayers = client.players();
        SyncSnapshotPlayers(snapshotPlayers, slots, assets);

        const VisualPlayer* localSlot = FindLocalPlayer(slots, client.joinInfo().clientId);
        static bool localFacingLeft = false;

        if (IsKeyPressed(KEY_F1)) {
            inGameSettings = !inGameSettings;
            if (inGameSettings) {
                chat.open = false;
                chat.draft.clear();
            }
        }

        if (!inGameSettings) {
            if (chat.open) {
                SetSoftKeyboardVisible(true);
                if (IsKeyPressed(KEY_ESCAPE)) {
                    chat.open = false;
                    chat.draft.clear();
                } else {
                    bool chatEnterPressed = HandleTextInput(chat.draft, false, false);
                    if (IsKeyPressed(KEY_BACKSPACE) && !chat.draft.empty()) {
                        chat.draft.pop_back();
                    }
                    if (IsKeyPressed(KEY_ENTER) || chatEnterPressed) {
                        client.sendChatMessage(chat.draft);
                        chat.open = false;
                        chat.draft.clear();
                    }
                }
            } else if (IsKeyPressed(KEY_SLASH) || IsKeyPressed(KEY_KP_DIVIDE)) {
                chat.open = true;
                chat.draft.clear();
#if defined(__ANDROID__)
                SetSoftKeyboardText(chat.draft.c_str());
#endif
                SetSoftKeyboardVisible(true);
            }
            if (!chat.open) {
                SetSoftKeyboardVisible(false);
            }
        } else {
            SetSoftKeyboardVisible(false);
        }

        bool moving = false;
        Vector2 move = {};
        if (!inGameSettings && !chat.open) {
            move = ReadMovementInput(joystick, settings.touchControls);
            moving = fabsf(move.x) > 0.01f || fabsf(move.y) > 0.01f;
            if (move.x < -0.01f) localFacingLeft = true;
            if (move.x > 0.01f) localFacingLeft = false;
            const int moveX = (move.x > 0.2f) - (move.x < -0.2f);
            const int moveY = (move.y > 0.2f) - (move.y < -0.2f);
            client.sendInput(moveX, moveY, moving, localFacingLeft);
        } else {
            client.sendInput(0, 0, false, localFacingLeft);
        }

        if (localSlot) {
            Vector2 target = PlayerCenter(localSlot->sprite);
            camera.target = target;
        }
        camera.offset = { (float)GetScreenWidth() * 0.5f, (float)GetScreenHeight() * 0.5f };

        vector<VisualPlayer*> drawOrder;
        drawOrder.reserve(4);
        for (auto& slot : slots) {
            if (slot.active) drawOrder.push_back(&slot);
        }
        sort(drawOrder.begin(), drawOrder.end(), [](const VisualPlayer* a, const VisualPlayer* b) {
            return a->sprite.y < b->sprite.y;
        });

        BeginDrawing();
        ClearBackground(Color{ 85, 92, 110, 255 });
        DrawCircleGradient(GetScreenWidth() / 2, GetScreenHeight() / 2, 900, Fade(BLACK, 0.0f), Fade(BLACK, 0.08f));
        BeginMode2D(camera);
        for (auto* slot : drawOrder) {
            slot->sprite.Animate(slot->sprite.moving);
            slot->sprite.Draw();
        }
        EndMode2D();

        DrawHud(client, ui, snapshotPlayers);
        DrawChatOverlay(client.chatLog(), chat);
        DrawRosterOverlay(snapshotPlayers);
        if (settings.touchControls) {
            joystick.Draw();
        }

        if (inGameSettings) {
            Rectangle fullscreenBtn = { 104, 180, 360, 48 };
            Rectangle touchBtn = { 104, 244, 360, 48 };
            Rectangle portMinus = { 104, 308, 48, 48 };
            Rectangle portPlus = { 416, 308, 48, 48 };
            Rectangle leaveBtn = { 104, 392, 152, 48 };
            Rectangle backBtn = { 270, 392, 152, 48 };

            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.45f));
            DrawSettingsPanel(settings, true);
            DrawText("Game is paused while settings are open.", 104, 132, 18, Fade(RAYWHITE, 0.8f));

            if (Button(fullscreenBtn, settings.fullscreen ? "Fullscreen: ON" : "Fullscreen: OFF")) {
                settings.fullscreen = !settings.fullscreen;
                ToggleFullscreen();
                settings.fullscreen = IsWindowFullscreen();
            }
            if (Button(touchBtn, settings.touchControls ? "Touch controls: ON" : "Touch controls: OFF")) {
                settings.touchControls = !settings.touchControls;
            }
            DrawText("Server port", (int)portMinus.x + 60, (int)portMinus.y + 14, 20, RAYWHITE);
            if (Button(portMinus, "-")) {
                settings.port = max(1, settings.port - 1);
            }
            if (Button(portPlus, "+")) {
                settings.port = min(65535, settings.port + 1);
            }
            DrawRectangleRounded({ 162, 308, 238, 48 }, 0.14f, 8, Fade(BLACK, 0.55f));
            DrawRectangleRoundedLines({ 162, 308, 238, 48 }, 0.14f, 8, 2.0f, Fade(WHITE, 0.3f));
            DrawText(TextFormat("%d", settings.port), 254, 322, 22, SKYBLUE);

            if (Button(leaveBtn, "Leave room")) {
                chat.open = false;
                chat.draft.clear();
                inGameSettings = false;
                client.disconnect();
                snapshotPlayers.clear();
                for (auto& slot : slots) slot.Clear();
                ui.error.clear();
                ui.message = "Enter an 8-character room code to join or create.";
                screen = ScreenState::Menu;
            }
            if (Button(backBtn, "Resume")) {
                inGameSettings = false;
            }
        }
        EndDrawing();
    }

    client.disconnect();
    for (auto& slot : slots) {
        slot.Clear();
    }
    CloseWindow();
    return 0;
}
