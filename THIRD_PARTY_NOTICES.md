# Third-party notices

## RNNoise

- Upstream: https://github.com/xiph/rnnoise
- Version: v0.1.1, commit `6cbfd53eb348a8d394e0757b4025c6ded34eb2b6`
- License: BSD 3-Clause; original copyright holders and text in `licenses/RNNoise-COPYING.txt` and `third_party/rnnoise/COPYING`.
- Original lightweight trained model is compiled into the application. Inference is local.
- Local MSVC changes: `_USE_MATH_DEFINES` enables `M_PI`; `pitch.c` and `celt_lpc.c` include `<malloc.h>` under `_MSC_VER` and use bounded stack `_alloca` instead of unsupported C99 variable-length arrays. Other builds retain original arrays. Upstream notices are preserved.

## Slint

- Upstream: https://github.com/slint-ui/slint
- Official C++ SDK/runtime: 1.17.1 Windows MSVC AMD64.
- SDK asset SHA-256: `f5b537da448c1e3d72a24a774e19518ae412b9706b8ef49bdee64b62b878fe56`.
- Application uses the Slint Royalty-free Desktop, Mobile, and Web Applications License 2.0, included in `licenses/Slint-Royalty-free-2.0.md`.
- Official `AboutSlint` widget is accessible in the App Settings screen. Do not remove it without providing another attribution option that satisfies the license.
- Runtime third-party dependency notices: `licenses/Slint-THIRDPARTY.md`. Slint framework license overview: `licenses/Slint-LICENSE.md`.
- Prebuilt SDK compiler and headers are not redistributed as a standalone SDK. The release contains the runtime integrated with QuietMic.

## Microsoft Visual C++ runtime

Release packaging may include Microsoft x64 CRT redistributable DLLs from the licensed Visual Studio installation's `VC/Redist/MSVC/.../x64/Microsoft.VC*.CRT` directory. Microsoft copyrights and signature notices are retained. These files are governed by Microsoft's Visual Studio redistributable terms; they are not licensed as QuietMic code.

Windows endpoint naming uses the Windows 10/11 AudioPolicy COM interface, isolated in `src/audio/audio_policy.hpp`. This interface is not part of the public Windows SDK; the endpoint's assigned name is checked through MMDevice after the call. ABI references: https://github.com/frgnca/AudioDeviceCmdlets/blob/master/SOURCE/IPolicyConfig.cs . No PureMic implementation is copied.

## NSIS installer

The installer is compiled with NSIS 3.12 (https://nsis.sourceforge.io/), using its zlib compressor. Original notices are preserved in `licenses/NSIS-COPYING.txt`. The compiler is a local build dependency, not an extra installation requirement for users.

## USB/IP transport client

- Upstream: https://github.com/vadimgrn/usbip-win2, release v.0.9.8.0.
- The three unmodified Windows x64 user-mode binaries in `third_party/usbip-win2/runtime` are redistributed under the BSD 2-Clause terms in `licenses/USBIP-BSD-2-Clause.txt`.
- Original release installer SHA-256: `81F426741F7EE2ED991FEBE24A22DACA8400B6AE2F171054E3FB404897E15D39`.
- `usbip.exe`: `0A562A2339DB09144209C762E25B7E67211070C12E87C65089C072AFB8D71BDC`.
- `libusbip.dll`: `600627D4AB4869D4231BD868047CD37022F5C651C318C8A5160699E93407B54B`.
- `resources.dll`: `F43F628C47BAFC284855B9C9FF4A42B5E6C2053AB3BC7A52CA951624C7C1AE76`.
- Their three imported VC runtime DLLs are staged alongside the child executable under Microsoft's redistributable terms above. The two original signed driver packages in `third_party/usbip-win2/driver-packages` are bundled separately and installed by QuietMic setup when missing. Copying the user-mode client alone does not install them.

## USB Audio Class reference

The local USB/IP and UAC1 implementation was developed with reference to https://github.com/tarekwasfy01/Virtual-Cables, including its audio descriptor layout. The BSD 2-Clause notice is retained in `licenses/VirtualCables-BSD-2-Clause.txt`. QuietMic uses its own C++ server, scheduling, lifecycle and RNNoise pipeline; the Go application is not bundled.
