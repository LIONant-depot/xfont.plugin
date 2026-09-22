#ifndef XFONT_EDITOR_H
#define XFONT_EDITOR_H
#pragma once

// The Font editor: opens a font resource from the asset browser in its own window. The descriptor is edited in an inspector and through commands
// (all undoable); the compiled font is shown as its atlas and as live text drawn through the MSDF shader. Hosts include this header and open
// editors through xeditor::open_resource_editors.
#include "source/Tools/Editor/xeditor_descriptor_editor.h"
#include "source/Tools/Editor/xeditor_camera.h"
#include "source/Examples/E05_Textures/E05_BitmapInspector.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_Resources.h"
#include "plugins/xfont.plugin/source/Editor/xfont_editor_preview.h"
#include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"
#include "plugins/xfont.plugin/source/xfont_xgpu_rsc_loader.cpp"        // the resource loader: compiled once, in the host's translation unit

#include <atomic>
#include <chrono>

namespace xfont_editor
{
    //--------------------------------------------------------------------------------------------
    // The editor
    //--------------------------------------------------------------------------------------------
    struct session : xeditor::descriptor_editor
    {
        // What the previews can show right now
        enum class state { none, not_compiled, reloading, ready };

        struct font_info_cmd : xundo::query_command_base
        {
            session& m_Session;
            font_info_cmd(xundo::system& System, session& Session) noexcept : query_command_base(System, "FontInfo", nullptr), m_Session(Session) {}
            const char* getCommandHelp() const noexcept override { return "What the compiled font holds: output type, glyphs, kerning, metrics, atlas size. Usage: FontInfo"; }
            void RegisterArguments() noexcept override {}
            std::string Query() noexcept override
            {
                static constexpr const char* s_State[] = { "nothing loaded", "not compiled yet", "reloading", "ready" };
                auto& S = m_Session;
                S.Resolve();
                auto* pFont = S.m_pFont;
                std::string Text = std::format("state: {}\n", s_State[static_cast<int>(S.m_State)]);
                if (S.m_State != state::ready || !pFont || !pFont->m_pFont) return Text;

                static constexpr const char* s_Type[] = { "MTSDF", "SDF", "BITMAP" };
                const auto& F = *pFont->m_pFont;
                Text += std::format("output type: {}\nglyphs: {}\nkern pairs: {}\nline height: {:.3f}\nascender: {:.3f}\ndescender: {:.3f}\npixel range: {:.2f}\n"
                    , s_Type[std::min<int>(static_cast<int>(F.m_OutputType), 2)], F.m_nGlyphs, F.m_nKernPairs, F.m_LineHeight, F.m_Ascender, F.m_Descender, F.m_PixelRange);
                if (pFont->m_pTexture)
                {
                    const auto Dims = pFont->m_pTexture->getTextureDimensions();
                    Text += std::format("atlas: {} x {}\n", Dims[0], Dims[1]);
                }
                return Text;
            }
        };

        xeditor::set_preview_cmd<render_settings>   m_SetPreview;
        xeditor::list_preview_cmd<render_settings>  m_ListPreview;
        xeditor::view2d_cmd             m_SetView;
        font_info_cmd                   m_FontInfo;

        render_settings                 m_Settings;
        xeditor::inspector_panel        m_SettingsInspector{ "Rendering Settings" };
        xeditor::inspector_panel        m_TextureInspector{ "Texture Info" };
        e05::bitmap_inspector           m_TextureInfo;              // what the font's atlas texture looks like on disk, shown by m_TextureInspector
        bool                            m_bTextureInfo = false;
        xresource::full_guid            m_TextureGuid;              // the font's virtual atlas texture, from what the compiler reported
        std::atomic<std::uint64_t>      m_TextureInstance = 0;      // the same, for the compile thread's callback
        std::atomic<bool>               m_bTextureCompiling = false;
        std::atomic<bool>               m_bTextureStarted = false;  // its compile began / finished: the next frame lets go / reloads
        std::atomic<bool>               m_bTextureDone = false;
        std::filesystem::file_time_type m_TextureStamp{};           // when the atlas file on disk was written, as of the load: a newer one means the atlas was compiled since
        int                             m_StampCheck = 0;
        std::chrono::steady_clock::time_point m_FontCompiledAt{};   // when the font's own compile finished: the atlas compile follows, or is not needed

        text_renderer                   m_Text;
        bool                            m_bText = false;            // the text renderer's GPU objects exist
        pan_zoom                        m_AtlasView, m_LiveView;

        xrsc::font_ref                  m_Ref;
        xfont::rt*                      m_pFont  = nullptr;         // the loaded font this frame (null unless m_State is ready)
        state                           m_State  = state::none;
        int                             m_ResolvedFrame = -1;
        std::chrono::steady_clock::time_point m_ReloadUntil{};      // the atlas is not touched again before this, after a compile: it is rebuilt behind our back

        session(xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) noexcept
            : descriptor_editor("Font", Guid, LibraryGuid, pDevice)
            , m_SetPreview(m_Undo, m_Settings), m_ListPreview(m_Undo, m_Settings), m_SetView(m_Undo, { { "atlas", { &m_AtlasView.m_Zoom, &m_AtlasView.m_Pan.m_X, &m_AtlasView.m_Pan.m_Y } }, { "live", { &m_LiveView.m_Zoom, &m_LiveView.m_Pan.m_X, &m_LiveView.m_Pan.m_Y } } }), m_FontInfo(m_Undo, *this)
        {
            m_SettingsInspector.BindObject(*xproperty::getObjectByType<render_settings>(), &m_Settings);

            AddPanel("Font Texture",       dock::left,        [this] { RenderAtlas(); });
            AddPanel("Texture Info",       dock::left_bottom, [this] { Resolve(); m_TextureInspector.Show(); });
            AddPanel("Font Live Preview",  dock::center,      [this] { RenderLive(); });
            AddPanel("Description",        dock::right,       [this] { m_DescriptorInspector.Show(); });
            AddPanel("Rendering Settings", dock::bottom,      [this] { m_Settings.m_CurrentFontOutputType = (m_pFont && m_pFont->m_pFont) ? m_pFont->m_pFont->m_OutputType : xfont_rsc::output_type::MTSDF; m_SettingsInspector.Show(); });

            m_bText = pDevice && m_Text.Create(*pDevice) == 0;
            e10::g_LibMgr.m_OnCompilationState.Register<&session::OnTextureCompilation>(*this);
            Reload();
        }

        ~session() noexcept override
        {
            e10::g_LibMgr.m_OnCompilationState.RemoveDelegates(this);
            xresource::g_Mgr.ReleaseRef(m_Ref);
            if (!m_pDevice) return;
            if (m_Text.m_SceneTexture.m_Private) xgpu::tools::imgui::ClearTexture(m_Text.m_SceneTexture);
            xeditor::DestroyGpu(m_pDevice, m_Text.m_PipelineInstance, m_Text.m_WirePipelineInstance, m_Text.m_Pipeline, m_Text.m_WirePipeline
                , m_Text.m_RenderPass, m_Text.m_SceneTexture, m_Text.m_DummyTexture);
        }

        // Once a frame, whichever tab is showing: the cooldown counts down and what the atlas' compile did is handled
        void Render() noexcept override
        {
            if (m_bTextureStarted.exchange(false)) LetGo();
            if (m_bTextureDone.exchange(false))    Reacquire();

            // The atlas compile does not always announce itself in time (or at all): a file written after we loaded it is a new atlas
            if (m_pFont && ++m_StampCheck % 15 == 0 && !m_pFont->m_TextureResourcePath.empty() && std::filesystem::exists(m_pFont->m_TextureResourcePath))
                if (const auto Stamp = std::filesystem::last_write_time(m_pFont->m_TextureResourcePath); m_TextureStamp != std::filesystem::file_time_type{} && Stamp != m_TextureStamp)
                    Reacquire();
            descriptor_editor::Render();
        }

        // The atlas is a resource of its own, compiled after the font (and not at all when nothing changed): its compile is followed the same way.
        void OnTextureCompilation(e10::library_mgr&, e10::library::guid, xresource::full_guid Compiling, std::shared_ptr<e10::compilation::historical_entry::log>& Log) noexcept
        {
            if (!Log || Compiling.m_Instance.m_Value != m_TextureInstance.load() || Compiling.m_Type != xrsc::texture_type_guid_v) return;
            e10::compilation::historical_entry::result Result;
            {
                xcontainer::lock::scope Lock(*Log);
                Result = Log->get().m_Result;
            }
            using result = e10::compilation::historical_entry::result;
            if (Result == result::COMPILING || Result == result::COMPILING_WARNINGS)
            {
                SetCompileStartTime(std::filesystem::file_time_type::clock::now());
                m_bTextureCompiling = true;
                m_bTextureStarted   = true;
            }
            else if (Result == result::SUCCESS || Result == result::SUCCESS_WARNINGS || Result == result::FAILURE)
            {
                m_bTextureCompiling = false;
                m_bTextureDone      = true;
            }
        }

        // The compiled files are about to be replaced: let go of them now, before this frame's panels touch a texture that is going away.
        void OnCompileStarted() noexcept override
        {
            LetGo();
        }

        void LetGo() noexcept
        {
            if (m_bText && m_pDevice) m_Text.Reset(*m_pDevice);
            xresource::g_Mgr.ReleaseRef(m_Ref);
            m_Ref.clear();
            m_pFont = nullptr;
            m_State = state::reloading;
            m_TextureInfo.clear();
            m_TextureInspector.Clear();
            m_bTextureInfo = false;
            m_ReloadUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);      // the compile and the reload are not synchronized to frames at all: generous, not tight
        }

        void OnCompiled() noexcept override
        {
            m_FontCompiledAt = std::chrono::steady_clock::now();
            Reacquire();
        }

        void Reacquire() noexcept
        {
            if (m_bText && m_pDevice) m_Text.Reset(*m_pDevice);
            Reload();
            m_ReloadUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }

        // Takes a reference to the compiled font again (the file may not exist yet: Resolve says so) and finds the atlas texture the compiler made for it.
        void Reload() noexcept
        {
            xresource::g_Mgr.ReleaseRef(m_Ref);
            m_Ref.clear();
            m_pFont = nullptr;
            m_TextureInfo.clear();
            m_TextureInspector.Clear();
            m_bTextureInfo = false;
            m_TextureGuid.clear();
            m_TextureInstance = 0;
            m_TextureStamp = {};
            m_ResolvedFrame = -1;
            if (!m_Document.isLoaded()) return;

            e10::g_LibMgr.getNodeInfo(m_Document.m_LibraryGuid, m_Document.m_Guid, [&](e10::library_db::info_node& Node)
            {
                if (!Node.m_Dependencies.m_VirtualResources.empty()) m_TextureGuid = Node.m_Dependencies.m_VirtualResources[0];     // a font emits exactly one
            });
            m_TextureInstance = m_TextureGuid.m_Instance.m_Value;
            if (!m_TextureGuid.empty())
                if (const auto Path = xresource::g_Mgr.getResourcePath(m_TextureGuid, L"Texture"); std::filesystem::exists(Path)) m_TextureStamp = std::filesystem::last_write_time(Path);
            m_Ref.m_Instance = m_Document.m_Guid.m_Instance;
        }

        // The texture's own numbers (size, compression, mips), once its file exists: it is compiled after the font.
        void LoadTextureInfo() noexcept
        {
            if (m_bTextureInfo || m_TextureGuid.empty()) return;
            const auto Path = xresource::g_Mgr.getResourcePath(m_TextureGuid, L"Texture");
            if (!std::filesystem::exists(Path)) return;
            m_TextureInfo.Load(Path, true);
            m_TextureInspector.BindObject(*xproperty::getObject(m_TextureInfo), &m_TextureInfo);
            m_bTextureInfo = true;
        }

        // What can be shown this frame: decided once per frame, however many panels ask.
        void Resolve() noexcept
        {
            const int Frame = ImGui::GetFrameCount();
            if (Frame == m_ResolvedFrame) return;
            m_ResolvedFrame = Frame;
            m_pFont = nullptr;
            m_State = state::none;
            if (!m_Document.isLoaded()) return;

            // A font that was just created has no compiled file yet: check before asking the manager (it asserts when a load fails).
            if (!std::filesystem::exists(m_Document.m_ResourcePath))  m_State = state::not_compiled;
            else if (m_Ref.empty())                                   m_State = state::reloading;
            else if (m_pFont = xresource::g_Mgr.getResource(m_Ref); !m_pFont) m_State = state::not_compiled;
            else if (std::chrono::steady_clock::now() < m_ReloadUntil) { m_State = state::reloading; m_pFont = nullptr; }
            else
            {
                // The font can be ready before its atlas is: the atlas is compiled after it, and the file of the last compile is still on disk.
                // Only a file written after this compile began is the new one. When the atlas has not started compiling shortly after the font
                // finished, nothing in it changed and the file on disk is the right one.
                const bool bWaiting = m_bTextureCompiling || std::chrono::steady_clock::now() - m_FontCompiledAt < std::chrono::seconds(3);
                const bool bStale = !m_pFont->m_TextureResourcePath.empty()
                    && (!std::filesystem::exists(m_pFont->m_TextureResourcePath) || (bWaiting && std::filesystem::last_write_time(m_pFont->m_TextureResourcePath) < CompileStartTime()));
                m_State = bStale ? state::reloading : state::ready;
                if (bStale) m_pFont = nullptr;
            }

            if (m_State == state::ready)
            {
                if (m_TextureStamp == std::filesystem::file_time_type{} && !m_pFont->m_TextureResourcePath.empty() && std::filesystem::exists(m_pFont->m_TextureResourcePath))
                    m_TextureStamp = std::filesystem::last_write_time(m_pFont->m_TextureResourcePath);
                LoadTextureInfo();
            }
        }

        bool ShowState() noexcept
        {
            switch (m_State)
            {
            case state::none:         ImGui::TextDisabled("Nothing loaded.");                   return false;
            case state::not_compiled: ImGui::TextDisabled("Not compiled yet - press Compile."); return false;
            case state::reloading:    ImGui::TextDisabled("(reloading...)");                    return false;
            case state::ready:        return true;
            }
            return false;
        }

        void RenderAtlas() noexcept
        {
            Resolve();
            if (!ShowState() || !m_pFont->m_pTexture) return;

            const auto Dims = m_pFont->m_pTexture->getTextureDimensions();
            std::vector<ImVec4> GlyphRects;
            if (m_Settings.m_bShowGlyphBounds) GlyphRects = CollectGlyphAtlasRects(*m_pFont->m_pFont);
            ShowZoomableImage("##AtlasView", static_cast<void*>(m_pFont->m_pTexture), Dims[0], Dims[1], m_AtlasView, ImVec2(0, 0), ImVec2(1, 1), m_Settings.m_bShowGlyphBounds ? &GlyphRects : nullptr);
        }

        void RenderLive() noexcept
        {
            Resolve();
            if (!ShowState()) return;
            auto* pHost   = xeditor::host::current();
            auto* pWindow = pHost ? pHost->find<xgpu::window>() : nullptr;
            if (!m_bText || !pWindow || !m_pDevice) { ImGui::TextDisabled("Live text needs a GPU device (open from E29)."); return; }

            const auto& Font = *m_pFont->m_pFont;
            ImGui::Text("Glyphs: %u   Kern pairs: %u", Font.m_nGlyphs, Font.m_nKernPairs);
            ImGui::Text("Line Height: %.3f   Ascender: %.3f   Descender: %.3f", Font.m_LineHeight, Font.m_Ascender, Font.m_Descender);
            ImGui::Separator();
            ImGui::Text("Live Text");
            if (!m_pFont->m_pTexture) { ImGui::TextDisabled("(texture not compiled yet)"); return; }

            const auto AtlasDims = m_pFont->m_pTexture->getTextureDimensions();

            // MTSDF and SDF are true distance fields, correct at any size, so the requested size is used as is. A BITMAP font only has the pixel
            // sizes it was baked at: its native size is the baked group closest to the request.
            std::vector<text_quad> Quads;
            float PenXFinal = 0.0f;
            float NativeScale = m_Settings.m_PreviewTextSize;
            if (Font.m_OutputType == xfont_rsc::output_type::BITMAP)
                if (const auto* pGroup = Font.FindClosestSizeGroup(m_Settings.m_PreviewTextSize)) NativeScale = pGroup->m_PixelSize;
            LayoutText(Font, AtlasDims[0], AtlasDims[1], m_Settings.m_Text, Quads, PenXFinal, NativeScale);

            // A camera over the text: the wheel zooms toward the cursor, dragging pans. The input is read before drawing, so this frame already shows it.
            ImGui::PushID("##LiveTextView");
            if (ImGui::SmallButton("Recenter")) m_LiveView.m_Pan = { 0.0f, 0.0f };
            ImGui::SameLine();
            ImGui::Text("Zoom:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##Zoom", &m_LiveView.m_Zoom, 0.01f, 0.05f, 40.0f, "%.2fx");

            ImGui::BeginChild("##canvas", ImGui::GetContentRegionAvail(), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
            const ImVec2 RawAvail   = ImGui::GetContentRegionAvail();
            const ImVec2 CanvasSize = ImVec2(std::max(RawAvail.x, 1.0f), std::max(RawAvail.y, 1.0f));        // InvisibleButton asserts on a zero size
            ImGui::InvisibleButton("##canvas_btn", CanvasSize);
            const ImVec2 CanvasMin = ImGui::GetItemRectMin();
            const bool   bHovered  = ImGui::IsItemHovered();

            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const ImVec2 Delta = ImGui::GetIO().MouseDelta;
                m_LiveView.m_Pan.m_X += Delta.x;
                m_LiveView.m_Pan.m_Y += Delta.y;
            }
            if (bHovered && ImGui::GetIO().MouseWheel != 0.0f)
            {
                // Keep the point under the cursor where it is: find it with the old camera, then solve for the pan that puts it back with the new zoom.
                const float  OldZoom = m_LiveView.m_Zoom;
                m_LiveView.m_Zoom = std::clamp(OldZoom * (1.0f + ImGui::GetIO().MouseWheel * 0.1f), 0.05f, 40.0f);
                const float  OldPxPerEm = NativeScale * OldZoom;
                const float  NewPxPerEm = NativeScale * m_LiveView.m_Zoom;
                const float  OldAnchorX = CanvasSize.x * 0.5f - PenXFinal * OldPxPerEm * 0.5f;
                const float  NewAnchorX = CanvasSize.x * 0.5f - PenXFinal * NewPxPerEm * 0.5f;
                const ImVec2 Mouse      = ImGui::GetIO().MousePos;
                const float  MouseX = Mouse.x - CanvasMin.x;
                const float  MouseY = Mouse.y - CanvasMin.y;
                const float  WorldX = (MouseX - OldAnchorX - m_LiveView.m_Pan.m_X) / OldPxPerEm;
                const float  WorldY = (CanvasSize.y * 0.5f + m_LiveView.m_Pan.m_Y - MouseY) / OldPxPerEm;
                m_LiveView.m_Pan.m_X = MouseX - NewAnchorX - WorldX * NewPxPerEm;
                m_LiveView.m_Pan.m_Y = MouseY - CanvasSize.y * 0.5f + WorldY * NewPxPerEm;
            }

            const int   ViewW   = std::max(static_cast<int>(CanvasSize.x), 8);
            const int   ViewH   = std::max(static_cast<int>(CanvasSize.y), 8);
            const float PxPerEm = NativeScale * m_LiveView.m_Zoom;

            // Every output type is one texture; the shader reads it as the type says (median of RGB for MTSDF, R for SDF, alpha for BITMAP)
            if (auto Err = m_Text.Draw
                ( *m_pDevice, *pWindow, Font
                , m_pFont->m_pTexture, m_pFont->m_pTexture
                , Quads, PenXFinal, ViewW, ViewH, PxPerEm, m_LiveView.m_Pan
                , m_Settings.m_bShowOutline, m_Settings.m_OutlineWidth
                , m_Settings.m_bBold, m_Settings.m_FontWeight
                , m_Settings.m_bShowShadow, xmath::fvec2{ m_Settings.m_ShadowOffsetX, m_Settings.m_ShadowOffsetY }
                , m_Settings.m_bBevel, m_Settings.m_BevelWeight
                , m_Settings.m_bGlow, m_Settings.m_GlowRadius, m_Settings.m_GlowIntensity
                , m_Settings.m_bItalic, m_Settings.m_ItalicShear
                , m_Settings.m_bShowGlyphBounds
                ); Err)
            {
                ImGui::TextDisabled("(text render failed: %d)", Err);
            }
            else if (m_Text.m_SceneW > 0 && m_Text.m_SceneH > 0)
            {
                // The scene texture is bigger than what was drawn (its size snaps to buckets): show the drawn part, over the whole canvas
                const ImVec2 UV1(static_cast<float>(m_Text.m_UsedW) / static_cast<float>(m_Text.m_SceneW), static_cast<float>(m_Text.m_UsedH) / static_cast<float>(m_Text.m_SceneH));
                ImGui::GetWindowDrawList()->AddImage(static_cast<void*>(&m_Text.m_SceneTexture), CanvasMin, ImVec2(CanvasMin.x + CanvasSize.x, CanvasMin.y + CanvasSize.y), ImVec2(0, 0), UV1);
            }
            ImGui::EndChild();
            ImGui::PopID();
        }
    };

    inline const xeditor::auto_register_resource_editor g_Registration
    { xrsc::font_type_guid_v
    , [](xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };
}

#endif // XFONT_EDITOR_H
