# sceSasCore NID and signature table

This is the source-owned disposition of every `sceSasCore` NID registered by
`src/rt/hle.c`. Signatures and parameter ranges follow the public
[PSPSDK header](https://raw.githubusercontent.com/pspdev/pspsdk/master/src/sascore/pspsascore.h)
and [SAS API documentation](https://pspdev.github.io/pspsdk/group__SAS.html).
The implementation column describes this runtime's state model, not a claim
of complete PSP audio emulation. `HST` is deliberately coarse: `yes` means a
bounded private route has exercised the import; `no` means the route census
reported no call; `unknown` means no public-safe call evidence is retained.

| NID | Canonical export | PSP signature | Nakagawa handler / interpretation | Disposition | HST |
| --- | --- | --- | --- | --- | --- |
| `0x42778A9F` | `__sceSasInit` | `(core, grain, maxvoices, outputmode, samplerate)` | `h_SasInit`; validates aligned full core span and retains configuration | CORRECT | yes |
| `0xA3589D81` | `__sceSasCore` | `(core, dst)` | `h_SasCore`; validates full destination grain, overwrites output, advances voices | PARTIAL (bounded mixer) | yes |
| `0x50A14DFC` | `__sceSasCoreWithMix` | `(core, dst, leftvol, rightvol)` | `h_SasCoreWithMix`; validates read/write span and adds scaled voice output | PARTIAL (bounded mixer) | yes |
| `0x68A46B95` | `__sceSasGetEndFlag` | `(core)` | `h_SasGetEndFlag`; returns ended bits for configured voices | CORRECT | yes |
| `0x76F01ACA` | `__sceSasSetKeyOn` | `(core, voice)` | `h_SasSetKeyOn`; starts the retained VAG/PCM/noise voice state | CORRECT | yes |
| `0xA0CF2FA4` | `__sceSasSetKeyOff` | `(core, voice)` | `h_SasSetKeyOff`; enters release without aliasing the voice index | CORRECT | unknown |
| `0x99944089` | `__sceSasSetVoice` | `(core, voice, vag, size, loop)` | `h_SasSetVoice`; validates complete VAG span and caller loop policy | CORRECT (VAG path) | yes |
| `0xE1CD9561` | `__sceSasSetVoicePCM` | `(core, voice, pcm, samplecount, loopstart)` | `h_SasSetVoicePCM`; validates complete mono signed-16 PCM span | PARTIAL (PCM path) | unknown |
| `0xAD84D37F` | `__sceSasSetPitch` | `(core, voice, pitch)` | `h_SasSetPitch`; validates `1..0x4000` and retains pitch | CORRECT | unknown |
| `0x440CA7D8` | `__sceSasSetVolume` | `(core, voice, left, right, sendleft, sendright)` | `h_SasSetVolume`; validates all four gains and retains dry/send gains | CORRECT | yes |
| `0x019B25EB` | `__sceSasSetADSR` | `(core, voice, mask, attack, decay, sustain, release)` | `h_SasSetADSR`; updates only fields selected by the four-bit mask | CORRECT (bounded envelope) | unknown |
| `0x9EC3676A` | `__sceSasSetADSRmode` | `(core, voice, mask, attack, decay, sustain, release)` | `h_SasSetADSRmode`; updates only selected curve modes | CORRECT (bounded envelope) | unknown |
| `0xCBCD4F79` | `__sceSasSetSimpleADSR` | `(core, voice, envelope1, envelope2)` | `h_SasSetSimpleADSR`; retains words and rejects the documented invalid bit | CORRECT (bounded envelope) | yes |
| `0x5F9529F6` | `__sceSasSetSL` | `(core, voice, level)` | `h_SasSetSL`; validates and retains sustain level | CORRECT | unknown |
| `0xB7660A23` | `__sceSasSetNoise` | `(core, voice, freq)` | `h_SasSetNoise`; selects an independent deterministic noise source | CORRECT (deterministic noise) | unknown |
| `0x33D4AB37` | `__sceSasRevType` | `(core, type)` | `h_SasRevType`; changes core effect type, never voice state | CORRECT (state only) | yes (route) |
| `0x267A6DD2` | `__sceSasRevParam` | `(core, delay, feedback)` | `h_SasRevParam`; validates and retains effect parameters | PARTIAL (state only) | unknown |
| `0xD5A229C9` | `__sceSasRevEVOL` | `(core, leftvol, rightvol)` | `h_SasRevEVOL`; validates and retains effect gains | PARTIAL (state only) | unknown |
| `0xF983B186` | `__sceSasRevVON` | `(core, dry, wet)` | `h_SasRevVON`; retains dry/wet enable state | PARTIAL (state only) | unknown |
| `0xE175EF66` | `__sceSasGetOutputmode` | `(core)` | `h_SasGetOutputmode`; returns retained output mode | CORRECT | unknown |
| `0xE855BF76` | `__sceSasSetOutputmode` | `(core, outputmode)` | `h_SasSetOutputmode`; validates stereo/multichannel mode | CORRECT | unknown |
| `0xD1E0A01E` | `__sceSasSetGrain` | `(core, grainsize)` | `h_SasSetGrain`; validates 64..2048 multiple-of-64 grain | CORRECT | unknown |
| `0xBD11B7C2` | `__sceSasGetGrain` | `(core)` | `h_SasGetGrain`; returns retained grain | CORRECT | unknown |
| `0x2C8E6AB3` | `__sceSasGetPauseFlag` | `(core)` | `h_SasGetPauseFlag`; returns retained pause bits | CORRECT | unknown |
| `0x787D04D5` | `__sceSasSetPause` | `(core, voicebit, pause)` | `h_SasSetPause`; updates selected pause bits | CORRECT | unknown |
| `0x74AE582A` | `__sceSasGetEnvelopeHeight` | `(core, voice)` | `h_SasGetEnvelopeHeight`; returns retained envelope height | CORRECT (bounded envelope) | unknown |
| `0x07F58C24` | `__sceSasGetAllEnvelopeHeights` | `(core, heights[32])` | `h_SasGetAllEnvelopeHeights`; validates and fills the complete 32-entry array | CORRECT | unknown |
| `0xA232CBE6` | `__sceSasSetTrianglarWave` | `(core, voice, parameter)` | `h_SasUnsupportedVoice`; validates core/voice, returns controlled invalid-state | UNIMPLEMENTED (controlled) | no evidence |
| `0xD5EBBBCD` | `__sceSasSetSteepWave` | `(core, voice, duty)` | `h_SasUnsupportedVoice`; validates core/voice, returns controlled invalid-state | UNIMPLEMENTED (controlled) | no evidence |
| `0x4AA9EAD6` | `__sceSasSetVoiceATRAC3` | `(core, voice, atrac3ctx)` | `h_SasUnsupportedVoice`; no fabricated ATRAC3 voice state | UNIMPLEMENTED (controlled) | no evidence |
| `0x7497EA85` | `__sceSasConcatenateATRAC3` | `(core, voice, data, size)` | `h_SasUnsupportedVoice`; no source read before controlled refusal | UNIMPLEMENTED (controlled) | no evidence |
| `0xF6107F00` | `__sceSasUnsetATRAC3` | `(core, voice)` | `h_SasUnsupportedVoice`; no fabricated ATRAC3 teardown | UNIMPLEMENTED (controlled) | no evidence |

The production-dispatch regression in `src/rt/hle_thread_selftest.c` covers the
two corrected routing pairs, independent noise activation, ADSR selection,
invalid-index non-aliasing, complete output spans, VAG loop policy, VAG output,
and `Core` versus `CoreWithMix` overwrite/add semantics. Unsupported waveform
and ATRAC3 operations remain explicit follow-up work rather than generic
success.
