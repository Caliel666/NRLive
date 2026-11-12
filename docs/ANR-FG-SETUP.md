# ANR Frame Generation runtime setup

NRLive calls ANR FG directly as an NGX D3D12 provider. The ANR runtime is not bundled with NRLive; download the official anr-fg-full package from the Apex Compute ANR releases:

https://github.com/apex-compute/ANR/releases

1. Extract the ANR package.
2. Copy the "Apex Neural Rendering FG" directory into the "anr" directory beside NRLive.exe.
3. Keep the package's internal folder structure intact. The provider DLL must end up at:

   NRLive\anr\Apex Neural Rendering FG\dlssg\nvngx_dlssg.dll

4. In NRLive Settings > General > Frame Generation, select ANR Frame Generation and restart NRLive.
5. Enable the FG toggle in the NRLive top bar.

NRLive searches only inside its own anr folder. It never looks for ANR in the target game's directory and does not install, replace, or modify game DLLs.

## Current integration limitations

- This first direct-provider path uses 2x output (one generated frame plus one real frame).
- NRLive currently supplies its optical-flow motion texture and synthetic depth texture. It does not have access to the game's native depth buffer, camera transforms, HUD-less color, or UI layers. Image quality and stability therefore depend on ANR accepting these estimates.
- The ANR provider is a closed beta runtime. ANR's source is not part of NRLive and its binary must be obtained from Apex Compute.
- If the provider fails to initialize or evaluate, NRLive reports the NGX error in NRLive.log and falls back to ordinary presentation. FSR remains unchanged as the default backend.
