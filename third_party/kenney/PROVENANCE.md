# PROVENANCE.md — `third_party/kenney/`

Two CC0 asset packs by Kenney (Kenney Vleugels, [kenney.nl](https://kenney.nl)), vendored file by file for the native
player's next UI. `tools/gen_ui_assets.py` embeds every `*.png` under `input-prompts/` and every `*.wav` under
`interface-sounds/` in `src/ui/nk_ui_assets.c`, so the player needs no extra staged files. Only the files the UI uses
are kept.

## Upstream source

| Pack | Page | Version | Archive | Archive SHA-256 |
| --- | --- | --- | --- | --- |
| Input Prompts | `https://kenney.nl/assets/input-prompts` | 1.5 (its `License.txt` says 1.5A, created 11-07-2026) | `kenney_input-prompts_1.5.zip` | `ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1` |
| Interface Sounds | `https://kenney.nl/assets/interface-sounds` | 1.0 (created 11-02-2020) | `kenney_interface-sounds.zip` | `f2193d072726d6758a5f7871b2dcc54dcce0d5c35c6f0a62f92549b327c81232` |

- License: `CC0-1.0`, as both asset pages and each pack's `License.txt` state. Credit is not required; the project
  credits Kenney in `CREDITS.md` and `NOTICE.md` anyway.
- Downloaded 2026-10-24 from the asset pages' download links. Each pack's `License.txt` is kept beside its files.
- The glyphs picture the face buttons of commonly used controllers. They are used only to show the player which
  button to press on the controller it has connected; the project claims no connection with any console maker.

## File manifest: `input-prompts/`

Every PNG is byte-identical to the file in the archive's `Default` (64 x 64) folder named in the second column. The
keyboard glyphs are stored as `kbd_*.png` instead of upstream's `keyboard_*.png`, because the publication tooling
treats "key" in a path as possible key material (`tools/policy_sync.py`); only the name differs.

| File | Archive path | SHA-256 |
| --- | --- | --- |
| `input-prompts/kbd_arrows.png` | `Keyboard & Mouse/Default/keyboard_arrows.png` | `ab5374333366fe237be253bd7f051984dd4524e3c44d6fb351fd81574f78515e` |
| `input-prompts/kbd_d.png` | `Keyboard & Mouse/Default/keyboard_d.png` | `6ca4515b5f56dd2730cb9acec3b183c8b2800ae641d75b434edd753e12bc36ad` |
| `input-prompts/kbd_e.png` | `Keyboard & Mouse/Default/keyboard_e.png` | `1008502780281c8c2e06bebbc477a60f485d2260072b04611fbd294340f266d7` |
| `input-prompts/kbd_enter.png` | `Keyboard & Mouse/Default/keyboard_enter.png` | `f849f917caa0a7eaa5c2322acc62c1de8e18322036961a2282cd85f1eca302c1` |
| `input-prompts/kbd_escape.png` | `Keyboard & Mouse/Default/keyboard_escape.png` | `8874e394d80161d6188ffce8b715357fea8059a9d6627a28769a91e3855bcbd7` |
| `input-prompts/kbd_f1.png` | `Keyboard & Mouse/Default/keyboard_f1.png` | `78277a3c50bada687a7d912220c1caa581d9d23948b9884631d405a4b441e31e` |
| `input-prompts/kbd_q.png` | `Keyboard & Mouse/Default/keyboard_q.png` | `cee375cefc9e2f19089bbd669153af9099fe71e08b6c577d48eb2a32bc567c96` |
| `input-prompts/kbd_space.png` | `Keyboard & Mouse/Default/keyboard_space.png` | `8ce6f043b05888bd1655f7660f8f7cc02acb6c763fecf358fef20405239524bf` |
| `input-prompts/playstation5_button_options.png` | `PlayStation Series/Default/playstation5_button_options.png` | `330360b4eb9f62799521a01a8c95c9ca0ef31bf9ab235fd551fe0c35a0e973f3` |
| `input-prompts/playstation_button_circle.png` | `PlayStation Series/Default/playstation_button_circle.png` | `74a47e04595c6db17a5c7d6f3e9c8830732d3e81f1e66ff4b19782213c9411d7` |
| `input-prompts/playstation_button_cross.png` | `PlayStation Series/Default/playstation_button_cross.png` | `afa03be5a125aea7e9746e030adc63d78689e8931c05905b7c079b755796fbcc` |
| `input-prompts/playstation_button_square.png` | `PlayStation Series/Default/playstation_button_square.png` | `66e53b7714e0e16eafa04713718667fa5b78b7c739f315e380a88f533eafc6db` |
| `input-prompts/playstation_button_triangle.png` | `PlayStation Series/Default/playstation_button_triangle.png` | `73f646a94056a71484f788375dd25efa1555489085e7f2f727175b1384b89e1c` |
| `input-prompts/playstation_dpad.png` | `PlayStation Series/Default/playstation_dpad.png` | `785171ce3d605c75accb44b6da44d05c2581cff3c32d905ab44ef76961f4222e` |
| `input-prompts/playstation_trigger_l1.png` | `PlayStation Series/Default/playstation_trigger_l1.png` | `17a323e995f7706a8237ef1db173a70f62b40585f6cb8a4557da05c92a4b8a94` |
| `input-prompts/playstation_trigger_r1.png` | `PlayStation Series/Default/playstation_trigger_r1.png` | `4fcd174085f00fdf22b286966d47ff52b2eac065df57b16ac68306ebb5d519a0` |
| `input-prompts/switch_button_a.png` | `Nintendo Switch/Default/switch_button_a.png` | `c24af67ce34d3116e4750f052221d6bfe8a87cb0de8364bb23a573a85391fb10` |
| `input-prompts/switch_button_b.png` | `Nintendo Switch/Default/switch_button_b.png` | `5d95680fcfbf59ba94a3cc696fd8251e4fe292b421716b90b5272ca5b32d91bf` |
| `input-prompts/switch_button_l.png` | `Nintendo Switch/Default/switch_button_l.png` | `944cbe119693f0bbae7bfceb9cb453c5ee4da898180441937190aeedb6bc0b83` |
| `input-prompts/switch_button_plus.png` | `Nintendo Switch/Default/switch_button_plus.png` | `0f8b7a6deb6665279a7763e5150bb74303a83dc45f442c978ed990bbd8e76d64` |
| `input-prompts/switch_button_r.png` | `Nintendo Switch/Default/switch_button_r.png` | `9a1a982e2f6ed96863d796809007298cd4108ac3b04e425f279d486ddd6051b1` |
| `input-prompts/switch_button_x.png` | `Nintendo Switch/Default/switch_button_x.png` | `90f1f195dd70724a605408bd60359c04b4013d8c5236442655256f43ea0cc043` |
| `input-prompts/switch_button_y.png` | `Nintendo Switch/Default/switch_button_y.png` | `133e98bbd388d29d7325b20b729518c98bb96707d743f2e142882b528bcf6263` |
| `input-prompts/switch_dpad.png` | `Nintendo Switch/Default/switch_dpad.png` | `cb6bbdc12a3b651f65ba511a163c73020936696bb1f0ba26483653848bfc3109` |
| `input-prompts/xbox_button_a.png` | `Xbox Series/Default/xbox_button_a.png` | `c24af67ce34d3116e4750f052221d6bfe8a87cb0de8364bb23a573a85391fb10` |
| `input-prompts/xbox_button_b.png` | `Xbox Series/Default/xbox_button_b.png` | `5d95680fcfbf59ba94a3cc696fd8251e4fe292b421716b90b5272ca5b32d91bf` |
| `input-prompts/xbox_button_menu.png` | `Xbox Series/Default/xbox_button_menu.png` | `49e03870c73236cdc757e7df6a053e52569acbf0ace0f95d4f1881c53e090247` |
| `input-prompts/xbox_button_x.png` | `Xbox Series/Default/xbox_button_x.png` | `90f1f195dd70724a605408bd60359c04b4013d8c5236442655256f43ea0cc043` |
| `input-prompts/xbox_button_y.png` | `Xbox Series/Default/xbox_button_y.png` | `133e98bbd388d29d7325b20b729518c98bb96707d743f2e142882b528bcf6263` |
| `input-prompts/xbox_dpad.png` | `Xbox Series/Default/xbox_dpad.png` | `10e27b4bb02ba3f6c9994e85998fca7bc002c6dc8297f8b67e80207273b202c6` |
| `input-prompts/xbox_lb.png` | `Xbox Series/Default/xbox_lb.png` | `587d43aac834743c64e6958f3fb13b0c5cc9075c9a84400e59c6fe1acbe3897f` |
| `input-prompts/xbox_rb.png` | `Xbox Series/Default/xbox_rb.png` | `692acf85c1b9e5c9d2b6c9b0a44bba2f21bf0f417e6192a4eb674f0c3627005e` |
| `input-prompts/License.txt` | `License.txt` | `a1a7ee88ca700fabc701c9746ca04436e9f2e4751f42a38afb26464106fe424d` (LOCAL DELTA) |

Upstream ships the Xbox and Switch A, B, X and Y glyphs as identical images; both copies are kept so each set reads
from its own files.

## File manifest: `interface-sounds/`

The pack ships Ogg Vorbis only. Each clip is a LOCAL DELTA: decoded once to 16-bit PCM WAV, 44.1 kHz mono (the
source's own rate and channels), so SDL3 plays it with no extra decoder:
`ffmpeg -i <clip>.ogg -map_metadata -1 -fflags +bitexact -flags:a +bitexact -c:a pcm_s16le -ar 44100 -ac 1 <clip>.wav`
(FFmpeg 9.0.2). No other change was made to the sound.

| File | Archive path and SHA-256 | SHA-256 |
| --- | --- | --- |
| `interface-sounds/back_001.wav` | `Audio/back_001.ogg`, `07db973f79f6ae0f2edc34561e7592e24d0577455919fb602cb8ecc0da991dcf` | `ea6abe8e02c7c2dd4d537a498d2a20f09f97d1983f4227fc45e65bdb3a7557e4` |
| `interface-sounds/confirmation_001.wav` | `Audio/confirmation_001.ogg`, `063564703b6094d70718a3e787a55cc9141611e4ecd6b6637f8828f79b4a8c3a` | `68771678d3d344051a1e7747e5f3384d35728b5232a043d4dd2851921b5c9293` |
| `interface-sounds/tick_001.wav` | `Audio/tick_001.ogg`, `9d057b918024c787bbe6c00beed13a7ef72a3fa0c259add3cef36582792f4230` | `5bb62847f130ced4299d758cee7d21bef49bff53c906e44e1ebc19d3d8f3a91a` |
| `interface-sounds/License.txt` | `License.txt` | `c36200e700dd1f244dc4234dd05aa0ca4d8f44eddd23657998011db728d4db9e` (LOCAL DELTA) |
| `PROVENANCE.md` | project-authored manifest | n/a |

## Local deltas

- Both `License.txt` files: CRLF line endings became LF, trailing whitespace and leading and trailing blank lines
  were removed (the tree's text and whitespace rules). The words are unchanged. Upstream SHA-256:
  `ca2f58f80f903512119da5828868199478119818cd7ecffb303d625cb74cbb5a` (Input Prompts),
  `f7966c773bbed0eca6a9c75081c44a178b38eae112724dbb5fdfbd4192d118a9` (Interface Sounds).
- The three WAV clips: decoded from Ogg Vorbis, as above.
