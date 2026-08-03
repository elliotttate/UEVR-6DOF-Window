# 6DOF Window Mode v0.2.0-alpha.1

This alpha turns the original soft mask into a genuinely room-anchored flat or
curved stereoscopic window and adds the optional CutsceneComfort bridge.

## New since v0.1.0-alpha.1

- The aperture is captured in OpenXR/OpenVR tracking space and no longer
  follows later head translation or rotation.
- Exact X width, Y height, and distance controls in physical meters.
- Optional 16:9 aspect lock.
- Physical feather width and rounded-corner radius.
- Symmetric horizontal curvature from flat to cylindrical.
- Configurable surround RGB color and opacity.
- Explicit recenter action and live anchor status in the UEVR menu.
- Generic `CutsceneComfort.WindowMode.v1` custom-event bridge. It applies
  transient cutscene settings without changing the user's normal all-game
  Window Mode configuration.
- A persistent D3D12 RTV per OpenXR color-swapchain image, avoiding descriptor
  aliasing while command lists are in flight.

## Validation boundary

- Release x64 backend build passes on the nightly-01139 base.
- Submitted-eye D3D11/OpenXR testing covered fixed-anchor movement, centered
  curvature, exact dimensions, feathering, rounded corners, and surround
  color in Meta XR Simulator.
- The matching D3D12 source path loads and reaches a live OpenXR session in
  Halo Campaign Evolved. A final submitted-eye D3D12 visual A/B remains open.

## Install

Extract the archive and run `UEVRInjector.exe` as you would the matching UEVR
nightly. In the in-game menu, open **6DOF Window**. The mode is off by default
and its settings are stored per game.

For cutscene-only activation, install the separate CutsceneComfort plugin in
the game's UEVR profile `plugins` folder.
