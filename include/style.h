#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <limits>

namespace vector_audio::style {
enum FrameType {
    FrameNormal = 0,
    FrameSelected,
    FramePrimary,
    FrameRadio,
    FrameTypeCount,
};

enum TextType {
    TextNormal = 0,
    TextBright,
    TextFailure,
    TextSuccess,
    TextLink,
    TextTypeCount,
};

static const ImColor backdrop = ImColor::HSV(8 / 14.f, 0.3f, 0.15f);

static const ImColor frame_normal[FrameTypeCount] = {
    ImColor::HSV(8 / 14.f, 0.3f, 0.2f),
    ImColor::HSV(8 / 14.f, 0.8f, 0.5f),
    ImColor::HSV(5 / 14.f, 0.8f, 0.5f),
    ImColor::HSV(2 / 14.f, 0.6f, 0.6f)
};

static const ImColor frame_hover[FrameTypeCount] = {
    ImColor::HSV(8 / 14.f, 0.3f, 0.3f),
    ImColor::HSV(8 / 14.f, 0.8f, 0.6f),
    ImColor::HSV(5 / 14.f, 0.8f, 0.6f),
    ImColor::HSV(2 / 14.f, 0.6f, 0.7f)
};

static const ImColor frame_active[FrameTypeCount] = {
    ImColor::HSV(8 / 14.f, 0.3f, 0.4f),
    ImColor::HSV(8 / 14.f, 0.8f, 0.7f),
    ImColor::HSV(5 / 14.f, 0.8f, 0.7f),
    ImColor::HSV(2 / 14.f, 0.6f, 0.8f)
};

static const ImColor text[TextTypeCount] = {
    ImColor(1.f, 1.f, 1.f, 0.75f),
    ImColor(1.f, 1.f, 1.f, 1.f),
    ImColor::HSV(0 / 14.f, 0.8f, 0.8f),
    ImColor::HSV(5 / 14.f, 0.8f, 0.8f),
    ImColor::HSV(8 / 14.f, 0.8f, 0.8f)
};

void PushFrameStyle(const FrameType i, bool interactive = true);
void PopFrameStyle();
void PushTextStyle(const TextType i);
void PopTextStyle();
void push_disabled_on(bool flag);
void pop_disabled_on(bool flag);

void UnroundCorners(char cw, bool button, bool interactive = true, float radius = 0.f);
void dualVUMeter(float fractionVu, float fractionPeak, const ImVec2& size_arg, ImColor vuColor, ImColor peakColor);

void apply();
}
