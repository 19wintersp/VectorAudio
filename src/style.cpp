#include "style.h"

namespace vector_audio::style {
void PushFrameStyle(const FrameType i, bool c) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, frame_normal[i].Value);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, (c ? frame_hover : frame_normal)[i].Value);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, (c ? frame_active : frame_normal)[i].Value);

    ImGui::PushStyleColor(ImGuiCol_Button, frame_normal[i].Value);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (c ? frame_hover : frame_normal)[i].Value);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, (c ? frame_active : frame_normal)[i].Value);
}

void PopFrameStyle() {
    ImGui::PopStyleColor(6);
}

void PushTextStyle(const TextType i) {
    ImGui::PushStyleColor(ImGuiCol_Text, text[i].Value);
}

void PopTextStyle() {
    ImGui::PopStyleColor();
}

void push_disabled_on(bool flag)
{
    if (!flag) return;

    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5);
};

void pop_disabled_on(bool flag)
{
    if (!flag) return;

    ImGui::PopItemFlag();
    ImGui::PopStyleVar();
};

float Saturate(float value) {
    return std::min(std::max(value, 0.0f), 1.0f);
}

void UnroundCorners(char cw, bool button, bool interactive, float radius) {
	if (radius == 0.f) radius = ImGui::GetStyle().FrameRounding;

	ImVec2 nw_min = ImGui::GetItemRectMin();
	ImVec2 se_max = ImGui::GetItemRectMax();

	ImVec2 nw_max(nw_min.x + radius, nw_min.y + radius);
	ImVec2 se_min(se_max.x - radius, se_max.y - radius);

	ImVec2 ne_min(se_min.x, nw_min.y);
	ImVec2 ne_max(se_max.x, nw_max.y);
	ImVec2 sw_min(nw_min.x, se_min.y);
	ImVec2 sw_max(nw_max.x, se_max.y);

	ImColor col(ImGui::GetStyleColorVec4(
		(interactive && ImGui::IsItemHovered())
			? (interactive && ImGui::IsItemActive())
				? (button ? ImGuiCol_ButtonActive : ImGuiCol_FrameBgActive)
				: (button ? ImGuiCol_ButtonHovered : ImGuiCol_FrameBgHovered)
			: (button ? ImGuiCol_Button : ImGuiCol_FrameBg)
	));

	if (cw & 8) ImGui::GetWindowDrawList()->AddRectFilled(nw_min, nw_max, col);
	if (cw & 4) ImGui::GetWindowDrawList()->AddRectFilled(ne_min, ne_max, col);
	if (cw & 2) ImGui::GetWindowDrawList()->AddRectFilled(se_min, se_max, col);
	if (cw & 1) ImGui::GetWindowDrawList()->AddRectFilled(sw_min, sw_max, col);
}

void dualVUMeter(float fractionVu, float fractionPeak, const ImVec2& size_arg, ImColor vuColor, ImColor peakColor)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size = ImGui::CalcItemSize(size_arg, ImGui::CalcItemWidth(), g.FontSize + style.FramePadding.y * 2.0f);
    ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(size, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, 0))
        return;

    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Render

    fractionVu = Saturate(fractionVu);
    fractionPeak = Saturate(fractionPeak);
    ImGui::RenderFrame(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), true, style.FrameRounding);
    bb.Expand(ImVec2(-style.FrameBorderSize, -style.FrameBorderSize));

    ImVec2 fill_br = ImVec2(ImLerp(bb.Min.x, bb.Max.x, fractionPeak), bb.Max.y);
    ImGui::RenderRectFilledRangeH(window->DrawList, bb, peakColor, 0.0f, fractionPeak, style.FrameRounding);

    fill_br = ImVec2(ImLerp(bb.Min.x, bb.Max.x, fractionVu), bb.Max.y);
    ImGui::RenderRectFilledRangeH(window->DrawList, bb, vuColor, 0.0f, fractionVu, style.FrameRounding);
}

void apply()
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.ChildBorderSize  = 0.f;
    style.DisabledAlpha    = 0.5f;
    style.FrameBorderSize  = 0.f;
    style.FramePadding     = ImVec2(6.f, 4.f);
    style.FrameRounding    = 4.f;
    style.GrabRounding     = 3.f; // sliders seem to use padding of 1.f?
    style.ItemSpacing      = ImVec2(8.f, 8.f);
    style.TabRounding      = 4.f;
    style.WindowBorderSize = 0.f;
    style.WindowPadding    = ImVec2(0.f, 0.f);

    colors[ImGuiCol_Text]                  = text[TextNormal].Value;
    colors[ImGuiCol_TextDisabled]          = text[TextNormal].Value; // op?
    colors[ImGuiCol_WindowBg]              = backdrop.Value;
    colors[ImGuiCol_ChildBg]               = ImVec4();
    colors[ImGuiCol_PopupBg]               = backdrop.Value;
    colors[ImGuiCol_FrameBg]               = frame_normal[FrameNormal].Value;
    colors[ImGuiCol_FrameBgHovered]        = frame_hover[FrameNormal].Value;
    colors[ImGuiCol_FrameBgActive]         = frame_active[FrameNormal].Value;
    colors[ImGuiCol_TitleBg]               = backdrop.Value;
    colors[ImGuiCol_TitleBgActive]         = frame_normal[FrameNormal].Value;
    colors[ImGuiCol_TitleBgCollapsed]      = backdrop.Value;
    colors[ImGuiCol_ScrollbarBg]           = backdrop.Value;
    colors[ImGuiCol_ScrollbarGrab]         = frame_normal[FrameNormal].Value;
    colors[ImGuiCol_ScrollbarGrabHovered]  = frame_hover[FrameNormal].Value;
    colors[ImGuiCol_ScrollbarGrabActive]   = frame_active[FrameNormal].Value;
    colors[ImGuiCol_CheckMark]             = text[TextNormal].Value;
    colors[ImGuiCol_SliderGrab]            = frame_normal[FrameSelected].Value;
    colors[ImGuiCol_SliderGrabActive]      = frame_active[FrameSelected].Value;
    colors[ImGuiCol_Button]                = frame_normal[FrameNormal].Value;
    colors[ImGuiCol_ButtonHovered]         = frame_hover[FrameNormal].Value;
    colors[ImGuiCol_ButtonActive]          = frame_active[FrameNormal].Value;
    colors[ImGuiCol_Header]                = frame_active[FrameNormal].Value;
    colors[ImGuiCol_HeaderHovered]         = frame_hover[FrameNormal].Value;
    colors[ImGuiCol_HeaderActive]          = frame_active[FrameNormal].Value;
    colors[ImGuiCol_Separator]             = text[TextNormal].Value;
    colors[ImGuiCol_Separator].w           = 0.25f;
    colors[ImGuiCol_SeparatorHovered]      = text[TextNormal].Value;
    colors[ImGuiCol_SeparatorActive]       = text[TextNormal].Value;
    colors[ImGuiCol_ResizeGrip]            = frame_normal[FrameNormal].Value;
    colors[ImGuiCol_ResizeGripHovered]     = frame_hover[FrameNormal].Value;
    colors[ImGuiCol_ResizeGripActive]      = frame_active[FrameNormal].Value;
    colors[ImGuiCol_Tab]                   = frame_normal[FrameNormal].Value;
    colors[ImGuiCol_TabHovered]            = frame_hover[FrameNormal].Value;
    colors[ImGuiCol_TabActive]             = frame_normal[FrameSelected].Value;
    colors[ImGuiCol_TabUnfocused]          = frame_normal[FrameNormal].Value;
    colors[ImGuiCol_TabUnfocusedActive]    = frame_normal[FrameSelected].Value;
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.f, 0.f, 0.f, 0.5f);
}
}
