#ifndef OBJECTS_HPP
#define OBJECTS_HPP

#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

using namespace std;

// Define your structures here
struct Player {
    float x = 0.0f;
    float y = 0.0f;
    float shadowX = 0.0f;
    float shadowY = 0.0f;
    float size = 1.0f;
    float speed = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    string name;

    Texture2D texture{};
    Texture2D shadow{};
    Rectangle rec{};
    Rectangle shadowRec{};
    bool ownsTextures = true;

    int frames = 1;
    int currentFrame = 0;
    float animationTimer = 0.0f;
    int framesSpeed = 8;
    bool hasShadow = false;
    bool facingLeft = false;
    bool moving = false;
    int colorIndex = 0;
    unsigned int clientId = 0;

    void Create(
            string NAME,
            const char *texturePath,
            float X, float Y,
            float SIZE,
            float SPEED,
            int FRAMES = 1,
            const char *shadowPath = nullptr,
            int FRAMES_SPEED = 8
    ) {
        name = std::move(NAME);
        x = X;
        y = Y;
        size = SIZE;
        speed = SPEED;
        frames = max(1, FRAMES);
        framesSpeed = max(1, FRAMES_SPEED);
        currentFrame = 0;
        animationTimer = 0.0f;

        texture = LoadTexture(texturePath);
        ownsTextures = true;
        width = (float)texture.width;
        height = (float)texture.height;
        rec = { 0.0f, 0.0f, width / frames, height };

        hasShadow = (shadowPath != nullptr);
        if (hasShadow) {
            shadow = LoadTexture(shadowPath);
            shadowRec = { 0.0f, 0.0f, (float)shadow.width, (float)shadow.height };
        }
    }

    void CreateFromTextures(
            string NAME,
            Texture2D textureHandle,
            Texture2D shadowHandle,
            float X, float Y,
            float SIZE,
            float SPEED,
            int FRAMES = 1,
            int FRAMES_SPEED = 8
    ) {
        name = std::move(NAME);
        x = X;
        y = Y;
        size = SIZE;
        speed = SPEED;
        frames = max(1, FRAMES);
        framesSpeed = max(1, FRAMES_SPEED);
        currentFrame = 0;
        animationTimer = 0.0f;

        texture = textureHandle;
        shadow = shadowHandle;
        ownsTextures = false;
        width = (float)texture.width;
        height = (float)texture.height;
        rec = { 0.0f, 0.0f, width / frames, height };

        hasShadow = (shadow.id != 0);
        if (hasShadow) {
            shadowRec = { 0.0f, 0.0f, (float)shadow.width, (float)shadow.height };
        }
    }

    void Animate(bool isMoving) {
        moving = isMoving;

        if (frames <= 1) {
            rec.x = 0.0f;
            return;
        }

        if (!moving) {
            currentFrame = 0;
            animationTimer = 0.0f;
            rec.x = 0.0f;
            return;
        }

        animationTimer += GetFrameTime();
        const float frameDuration = 1.0f / (float)framesSpeed;
        if (animationTimer >= frameDuration) {
            animationTimer = 0.0f;
            currentFrame++;
            if (currentFrame >= frames) currentFrame = 1;
        }

        rec.x = (float)currentFrame * (width / frames);
    }

    void Draw() {
        const int fontSize = max(12, (int)(16.0f * size / 0.2f));
        const float playerWidth = fabsf(rec.width) * size;
        const int textWidth = MeasureText(name.empty() ? "Guest" : name.c_str(), fontSize);
        const float textX = x + (playerWidth - (float)textWidth) * 0.5f;
        const float textY = y - (float)fontSize - 4.0f;

        DrawText(name.empty() ? "Guest" : name.c_str(), (int)textX, (int)textY, fontSize, WHITE);
        if (hasShadow) DrawShadow();
        DrawPlayer();
    }

    void DrawShadow() {
        const float drawnPlayerWidth = fabsf(rec.width) * size;
        const float drawnPlayerHeight = rec.height * size;
        const float drawnShadowWidth = shadow.width * size * 1.7f;
        const float drawnShadowHeight = shadow.height * size * 1.7f;

        shadowX = x + (drawnPlayerWidth - drawnShadowWidth) / 2.0f;
        shadowY = y + drawnPlayerHeight - drawnShadowHeight;

        Rectangle dest = { shadowX, shadowY, drawnShadowWidth, drawnShadowHeight };
        DrawTexturePro(shadow, shadowRec, dest, { 0, 0 }, 0.0f, Fade(WHITE, 0.7f));
    }

    void DrawPlayer() {
        Rectangle dest = { x, y, fabsf(rec.width) * size, rec.height * size };
        DrawTexturePro(texture, rec, dest, { 0, 0 }, 0.0f, WHITE);
    }

    void Unload() {
        if (ownsTextures) {
            if (texture.id != 0) UnloadTexture(texture);
            if (hasShadow && shadow.id != 0) UnloadTexture(shadow);
        }
        texture = {};
        shadow = {};
        ownsTextures = true;
    }

    void flipRight() {
        rec.width = fabsf(rec.width);
        facingLeft = false;
    }

    void flipLeft() {
        rec.width = -fabsf(rec.width);
        facingLeft = true;
    }

    void SetFacing(bool left) {
        if (left) flipLeft();
        else flipRight();
    }

    void SetState(float X, float Y, bool isMoving, bool left) {
        x = X;
        y = Y;
        SetFacing(left);
        Animate(isMoving);
    }
};

// Declare your function prototypes (notice the semicolons at the end)
void UpdatePlayerMovement(Player& player);

#endif // OBJECTS_HPP
