#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/WallpaperState.h"

using WallpaperEngine::Render::WallpaperState;

TEST_CASE ("parseScalingMode maps CLI/hotswap names to their enum value") {
    CHECK (WallpaperState::parseScalingMode ("stretch") == WallpaperState::TextureUVsScaling::StretchUVs);
    CHECK (WallpaperState::parseScalingMode ("fit") == WallpaperState::TextureUVsScaling::ZoomFitUVs);
    CHECK (WallpaperState::parseScalingMode ("fill") == WallpaperState::TextureUVsScaling::ZoomFillUVs);
    CHECK (WallpaperState::parseScalingMode ("center") == WallpaperState::TextureUVsScaling::CenterUVs);
    CHECK (WallpaperState::parseScalingMode ("default") == WallpaperState::TextureUVsScaling::DefaultUVs);
    CHECK_FALSE (WallpaperState::parseScalingMode ("bogus").has_value ());
}

TEST_CASE ("Center scaling crops a wallpaper bigger than the viewport") {
    WallpaperState state (WallpaperState::TextureUVsScaling::CenterUVs, 0);

    // 1000x1000 wallpaper on a 500x500 viewport: should show the centered 500x500 slice
    state.updateState ({ 0, 0, 500, 500 }, false, 1000, 1000);

    const auto [ustart, uend, vstart, vend] = state.getTextureUVs ();

    CHECK (ustart == Catch::Approx (0.25f));
    CHECK (uend == Catch::Approx (0.75f));
    CHECK (vend == Catch::Approx (0.25f));
    CHECK (vstart == Catch::Approx (0.75f));
}

TEST_CASE ("Center scaling letterboxes a wallpaper smaller than the viewport") {
    WallpaperState state (WallpaperState::TextureUVsScaling::CenterUVs, 0);

    // 500x500 wallpaper on a 1000x1000 viewport: UVs overflow past [0,1] so border clamping
    // can paint the letterbox/pillarbox around the centered, unscaled image
    state.updateState ({ 0, 0, 1000, 1000 }, false, 500, 500);

    const auto [ustart, uend, vstart, vend] = state.getTextureUVs ();

    CHECK (ustart == Catch::Approx (-0.5f));
    CHECK (uend == Catch::Approx (1.5f));
    CHECK (vend == Catch::Approx (-0.5f));
    CHECK (vstart == Catch::Approx (1.5f));
}

TEST_CASE ("Changing scaling mode live is picked up even if the viewport hasn't changed") {
    WallpaperState state (WallpaperState::TextureUVsScaling::StretchUVs, 0);

    state.updateState ({ 0, 0, 500, 500 }, false, 1000, 1000);
    CHECK_FALSE (state.hasChanged ({ 0, 0, 500, 500 }, false, 1000, 1000));

    state.setTextureUVsStrategy (WallpaperState::TextureUVsScaling::CenterUVs);

    CHECK (state.hasChanged ({ 0, 0, 500, 500 }, false, 1000, 1000));

    state.updateState ({ 0, 0, 500, 500 }, false, 1000, 1000);

    const auto [ustart, uend, vstart, vend] = state.getTextureUVs ();

    CHECK (ustart == Catch::Approx (0.25f));
    CHECK (uend == Catch::Approx (0.75f));
    CHECK_FALSE (state.hasChanged ({ 0, 0, 500, 500 }, false, 1000, 1000));
}

TEST_CASE ("setZoom clamps to a sane range") {
    WallpaperState state (WallpaperState::TextureUVsScaling::StretchUVs, 0);

    CHECK (state.getZoom () == Catch::Approx (1.0f));

    state.setZoom (100.0f);
    CHECK (state.getZoom () == Catch::Approx (5.0f));

    state.setZoom (0.0f);
    CHECK (state.getZoom () == Catch::Approx (0.1f));
}

TEST_CASE ("Zooming in crops tighter around the center of whatever the scaling mode picked") {
    WallpaperState state (WallpaperState::TextureUVsScaling::StretchUVs, 0);
    state.setZoom (2.0f);

    state.updateState ({ 0, 0, 1000, 1000 }, false, 1000, 1000);

    const auto [ustart, uend, vstart, vend] = state.getTextureUVs ();

    CHECK (ustart == Catch::Approx (0.25f));
    CHECK (uend == Catch::Approx (0.75f));
    CHECK (vstart == Catch::Approx (0.75f));
    CHECK (vend == Catch::Approx (0.25f));
}

TEST_CASE ("Zooming out overflows past [0,1] so border clamping can reveal more of the wallpaper") {
    WallpaperState state (WallpaperState::TextureUVsScaling::StretchUVs, 0);
    state.setZoom (0.5f);

    state.updateState ({ 0, 0, 1000, 1000 }, false, 1000, 1000);

    const auto [ustart, uend, vstart, vend] = state.getTextureUVs ();

    CHECK (ustart == Catch::Approx (-0.5f));
    CHECK (uend == Catch::Approx (1.5f));
    CHECK (vstart == Catch::Approx (1.5f));
    CHECK (vend == Catch::Approx (-0.5f));
}

TEST_CASE ("Changing zoom live is picked up even if the viewport hasn't changed") {
    WallpaperState state (WallpaperState::TextureUVsScaling::StretchUVs, 0);

    state.updateState ({ 0, 0, 1000, 1000 }, false, 1000, 1000);
    CHECK_FALSE (state.hasChanged ({ 0, 0, 1000, 1000 }, false, 1000, 1000));

    state.setZoom (2.0f);

    CHECK (state.hasChanged ({ 0, 0, 1000, 1000 }, false, 1000, 1000));
}

TEST_CASE ("setOffset clamps to [-1, 1]") {
    WallpaperState state (WallpaperState::TextureUVsScaling::CenterUVs, 0);

    state.setOffset (5.0f, -5.0f);
    CHECK (state.getOffsetX () == Catch::Approx (1.0f));
    CHECK (state.getOffsetY () == Catch::Approx (-1.0f));
}

TEST_CASE ("Offset has no effect when nothing is cropped") {
    WallpaperState state (WallpaperState::TextureUVsScaling::StretchUVs, 0);
    state.setOffset (1.0f, 1.0f);

    state.updateState ({ 0, 0, 1000, 1000 }, false, 1000, 1000);

    const auto [ustart, uend, vstart, vend] = state.getTextureUVs ();

    CHECK (ustart == Catch::Approx (0.0f));
    CHECK (uend == Catch::Approx (1.0f));
    CHECK (vstart == Catch::Approx (1.0f));
    CHECK (vend == Catch::Approx (0.0f));
}

TEST_CASE ("Offset slides a cropped window toward one edge without changing its size") {
    WallpaperState state (WallpaperState::TextureUVsScaling::CenterUVs, 0);
    state.setOffset (1.0f, 0.0f);

    // offsetX=1 should push the centered crop window fully to the right edge, Y untouched
    state.updateState ({ 0, 0, 500, 500 }, false, 1000, 1000);

    const auto [ustart, uend, vstart, vend] = state.getTextureUVs ();

    CHECK (ustart == Catch::Approx (0.5f));
    CHECK (uend == Catch::Approx (1.0f));
    CHECK (vend == Catch::Approx (0.25f));
    CHECK (vstart == Catch::Approx (0.75f));
}

TEST_CASE ("Changing offset live is picked up even if the viewport hasn't changed") {
    WallpaperState state (WallpaperState::TextureUVsScaling::CenterUVs, 0);

    state.updateState ({ 0, 0, 500, 500 }, false, 1000, 1000);
    CHECK_FALSE (state.hasChanged ({ 0, 0, 500, 500 }, false, 1000, 1000));

    state.setOffset (1.0f, 0.0f);

    CHECK (state.hasChanged ({ 0, 0, 500, 500 }, false, 1000, 1000));
}
