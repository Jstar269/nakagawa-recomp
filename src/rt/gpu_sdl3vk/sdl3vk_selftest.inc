// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * sdl3vk's self-tests, kept out of sdl3vk.c so the renderer reads as the renderer.
 *
 * This is not a translation unit: sdl3vk.c includes it at the point where the code used to
 * sit, because the tests reach the renderer's file-local state (swapchain, frame ring, init
 * fault seams) directly. Build it only through sdl3vk.c. It holds the capture self-test
 * (issue #57), the input-profile and pause-input self-tests, and the lifecycle self-test
 * (#1377); the entry point is sdl3vk_capture_selftest(), run by gpu_capture_selftest.c.
 */

/* ---- capture selftest (issue #57) ---------------------------------------------------- */

/* Scratch root for the files the capture selftest writes (see sdl3vk_capture_selftest). The
 * Makefile passes -DSR_SELFTEST_BUILD_ROOT=$(BUILD_ROOT); a bare compile keeps "build". */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif
#ifdef _WIN32
#define SDL3VK_TEST_CHDIR(path) _chdir(path)
#else
#define SDL3VK_TEST_CHDIR(path) chdir(path)
#endif

static int cap_test_pattern(int x, int y, int ch) {
    /* Deterministic per-channel pattern across the whole 8-bit range; every channel
     * varies on both axes, so row-pitch shears and channel-order mistakes change bytes. */
    switch (ch) {
    case 0: return (x * 7 + y * 3) & 0xff;
    case 1: return (x * 5 + y * 11) & 0xff;
    default: return (x * 13 + y * 17 + 41) & 0xff;
    }
}

/* Sets a variable that sdl3vk_init's getenv reads. On Windows that is the C runtime's copy:
 * _putenv_s updates it and the process environment, while SetEnvironmentVariableA alone changes
 * only the process environment, which getenv never sees again (#1377). */
static int cap_test_setenv(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

static void profile_test_setenv(const char *name, const char *value) {
#ifdef _WIN32
    if (value && value[0]) {
        _putenv_s(name, value);
    } else {
        _putenv_s(name, "");
    }
#else
    if (value && value[0]) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

static int cap_test_rmdir(const char *path) {
#ifdef _WIN32
    return _rmdir(path) == 0;
#else
    return rmdir(path) == 0;
#endif
}

/* Byte-exact P6 verification: exact header, exact payload, no trailing bytes. */
static int cap_test_verify_ppm(const char *path, int w, int h) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char hdr[64];
    int ok = 0;
    {
        char expect[64];
        snprintf(expect, sizeof expect, "P6\n%d %d\n255\n", w, h);
        size_t hl = strlen(expect);
        size_t got = fread(hdr, 1, hl, f);
        hdr[got] = '\0';
        if (got != hl || memcmp(hdr, expect, hl) != 0) {
            fprintf(stderr, "verify: header mismatch (got %zu bytes)\n", got);
            for (size_t i = 0; i < got; i++)
                fprintf(stderr, "  hdr byte %02u: %02x\n", (unsigned)i, (unsigned char)hdr[i]);
            return 0;
        }
            size_t body = (size_t)w * (size_t)h * 3u;
            uint8_t *buf = (uint8_t *)malloc(body);
            if (buf) {
                size_t got = fread(buf, 1, body, f);
                int extra = fgetc(f);
                if (got == body && extra == EOF) {
                    ok = 1;
                    for (int y = 0; y < h && ok; y++)
                        for (int x = 0; x < w && ok; x++) {
                            const uint8_t *px = buf + ((size_t)y * (size_t)w + x) * 3u;
                            if (px[0] != (uint8_t)cap_test_pattern(x, y, 0) ||
                                px[1] != (uint8_t)cap_test_pattern(x, y, 1) ||
                                px[2] != (uint8_t)cap_test_pattern(x, y, 2)) {
                                fprintf(stderr, "verify: first mismatch at (%d,%d) got "
                                        "%02x%02x%02x want %02x%02x%02x\n",
                                        x, y, px[0], px[1], px[2],
                                        cap_test_pattern(x, y, 0), cap_test_pattern(x, y, 1),
                                        cap_test_pattern(x, y, 2));
                                ok = 0;
                            }
                        }
                } else {
                    fprintf(stderr, "verify: body got %zu want %zu, extra=%d\n",
                            got, body, extra);
                }
                free(buf);
            }
    }
    fclose(f);
    return ok;
}

static void cap_test_fill_cpu(uint32_t *px, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)y * (size_t)w + x] = 0xFF000000u |
                ((uint32_t)cap_test_pattern(x, y, 0) << 16) |
                ((uint32_t)cap_test_pattern(x, y, 1) << 8) |
                (uint32_t)cap_test_pattern(x, y, 2);
}

typedef struct CapTestImage {
    VkImage img;
    VkDeviceMemory mem;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    void *staging_map;
    int w, h;
} CapTestImage;

static void cap_test_image_destroy(CapTestImage *t) {
    if (t->staging_map) vkUnmapMemory(s_dev, t->staging_mem);
    if (t->staging)     vkDestroyBuffer(s_dev, t->staging, NULL);
    if (t->staging_mem) vkFreeMemory(s_dev, t->staging_mem, NULL);
    if (t->img)         vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem)         vkFreeMemory(s_dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}

/* An R8G8B8A8 image in the exact shape ge_gpu.c presents: TRANSFER_DST|TRANSFER_SRC. */
static int cap_test_image_create(CapTestImage *t, int w, int h) {
    memset(t, 0, sizeof *t);
    t->w = w; t->h = h;
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = VK_FORMAT_R8G8B8A8_UNORM;
    imi.extent.width = (uint32_t)w; imi.extent.height = (uint32_t)h; imi.extent.depth = 1;
    imi.mipLevels = 1; imi.arrayLayers = 1; imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &imi, NULL, &t->img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(s_dev, t->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s_dev, &mai, NULL, &t->mem) != VK_SUCCESS ||
        vkBindImageMemory(s_dev, t->img, t->mem, 0) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)((size_t)w * (size_t)h * 4u);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &t->staging) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    vkGetBufferMemoryRequirements(s_dev, t->staging, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s_dev, &mai, NULL, &t->staging_mem) != VK_SUCCESS ||
        vkBindBufferMemory(s_dev, t->staging, t->staging_mem, 0) != VK_SUCCESS ||
        vkMapMemory(s_dev, t->staging_mem, 0, VK_WHOLE_SIZE, 0, &t->staging_map) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    return 1;
}

static int cap_test_image_submit(CapTestImage *t);

/* Upload the pattern and leave the image TRANSFER_SRC_OPTIMAL, exactly the contract
 * sdl3vk_present_image(_ex) requires of GE targets. */
static int cap_test_image_upload(CapTestImage *t) {
    uint8_t *dst = (uint8_t *)t->staging_map;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            uint8_t *px = dst + ((size_t)y * (size_t)t->w + x) * 4u;
            px[0] = (uint8_t)cap_test_pattern(x, y, 0);
            px[1] = (uint8_t)cap_test_pattern(x, y, 1);
            px[2] = (uint8_t)cap_test_pattern(x, y, 2);
            px[3] = 0xFF;
        }
    return cap_test_image_submit(t);
}

/* A dark frame for the picture probe: every colour byte 0 except the last pixel's first
 * colour byte, `last`, so the probe's brightest byte is `last` and only a scan of the whole
 * frame finds it. The ignored byte is 0xFF everywhere and never counts. */
static int cap_test_image_upload_dark(CapTestImage *t, uint8_t last) {
    uint8_t *dst = (uint8_t *)t->staging_map;
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; i++) {
        dst[i * 4u + 0u] = 0u;
        dst[i * 4u + 1u] = 0u;
        dst[i * 4u + 2u] = 0u;
        dst[i * 4u + 3u] = 0xFFu;
    }
    dst[(n - 1u) * 4u] = last;
    return cap_test_image_submit(t);
}

static int cap_test_image_submit(CapTestImage *t) {
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s_cmd, &bi) != VK_SUCCESS) return 0;
    barrier(s_cmd, t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)t->w; bic.imageExtent.height = (uint32_t)t->h;
    bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, t->staging, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    barrier(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (vkEndCommandBuffer(s_cmd) != VK_SUCCESS) return 0;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cmd;
    uint64_t perf_submit_started = sr_perf_now_ns();
    if (vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return 0;
    if (perf_submit_started) sr_perf_vulkan_submit(perf_submit_started);
    uint64_t perf_wait_started = sr_perf_now_ns();
    if (vkQueueWaitIdle(s_queue) != VK_SUCCESS) return 0;
    if (perf_wait_started) sr_perf_vulkan_wait(perf_wait_started, 0);
    return 1;
}

/* Its fixture names are relative: sdl3vk_capture_selftest runs this from the scratch working
 * directory beneath SR_SELFTEST_BUILD_ROOT, so the files never land in the checkout. */
int sdl3vk_input_profile_selftest(void) {
    int ok = 1;

    /* ---- 5(a) No profile: sampled PSP buttons and analog bytes equal old hard-coded mapping ---- */
    {
        static const struct {
            NkHostGamepadButton host_btn;
            uint32_t expected_bit;
        } kButtonTable[] = {
            { NK_HOST_BUTTON_SOUTH,          0x4000 }, /* CROSS */
            { NK_HOST_BUTTON_EAST,           0x2000 }, /* CIRCLE */
            { NK_HOST_BUTTON_WEST,           0x8000 }, /* SQUARE */
            { NK_HOST_BUTTON_NORTH,          0x1000 }, /* TRIANGLE */
            { NK_HOST_BUTTON_START,          0x0008 }, /* START */
            { NK_HOST_BUTTON_BACK,           0x0001 }, /* SELECT */
            { NK_HOST_BUTTON_LEFT_SHOULDER,  0x0100 }, /* LTRIGGER */
            { NK_HOST_BUTTON_RIGHT_SHOULDER, 0x0200 }, /* RTRIGGER */
            { NK_HOST_BUTTON_DPAD_UP,        0x0010 }, /* UP */
            { NK_HOST_BUTTON_DPAD_DOWN,      0x0040 }, /* DOWN */
            { NK_HOST_BUTTON_DPAD_LEFT,      0x0080 }, /* LEFT */
            { NK_HOST_BUTTON_DPAD_RIGHT,     0x0020 }, /* RIGHT */
        };

        for (size_t i = 0; i < sizeof(kButtonTable) / sizeof(kButtonTable[0]); i++) {
            bool btns[NK_HOST_BUTTON_COUNT] = {false};
            int16_t axes[NK_HOST_AXIS_COUNT] = {0};
            btns[kButtonTable[i].host_btn] = true;
            uint32_t out_b = 0;
            uint8_t out_lx = 0, out_ly = 0;
            sdl3vk_map_gamepad(NULL, btns, axes, &out_b, &out_lx, &out_ly);
            if (out_b != kButtonTable[i].expected_bit) {
                fprintf(stderr, "input_profile_selftest: button %d failed: got 0x%04x expected 0x%04x\n",
                        kButtonTable[i].host_btn, out_b, kButtonTable[i].expected_bit);
                ok = 0;
            }
            if (out_lx != 128 || out_ly != 128) {
                fprintf(stderr, "input_profile_selftest: neutral analog failed on button press\n");
                ok = 0;
            }
        }

        static const NkHostGamepadButton kUnmapped[] = {
            NK_HOST_BUTTON_GUIDE, NK_HOST_BUTTON_LEFT_STICK, NK_HOST_BUTTON_RIGHT_STICK,
            NK_HOST_BUTTON_MISC1, NK_HOST_BUTTON_RIGHT_PADDLE1, NK_HOST_BUTTON_LEFT_PADDLE1,
            NK_HOST_BUTTON_RIGHT_PADDLE2, NK_HOST_BUTTON_LEFT_PADDLE2, NK_HOST_BUTTON_TOUCHPAD
        };
        for (size_t i = 0; i < sizeof(kUnmapped) / sizeof(kUnmapped[0]); i++) {
            bool btns[NK_HOST_BUTTON_COUNT] = {false};
            int16_t axes[NK_HOST_AXIS_COUNT] = {0};
            btns[kUnmapped[i]] = true;
            uint32_t out_b = 0;
            sdl3vk_map_gamepad(NULL, btns, axes, &out_b, NULL, NULL);
            if (out_b != 0) {
                fprintf(stderr, "input_profile_selftest: unmapped button %d produced 0x%04x\n",
                        kUnmapped[i], out_b);
                ok = 0;
            }
        }

        int16_t trig_samples[] = {-32768, 0, 8191, 8192, 8193, 16384, 32767};
        for (size_t i = 0; i < sizeof(trig_samples) / sizeof(trig_samples[0]); i++) {
            bool btns[NK_HOST_BUTTON_COUNT] = {false};
            int16_t axes[NK_HOST_AXIS_COUNT] = {0};
            axes[NK_HOST_AXIS_LEFT_TRIGGER] = trig_samples[i];
            axes[NK_HOST_AXIS_RIGHT_TRIGGER] = trig_samples[i];
            uint32_t out_b = 0;
            sdl3vk_map_gamepad(NULL, btns, axes, &out_b, NULL, NULL);
            uint32_t expected = 0;
            if (trig_samples[i] > 8192) expected |= (0x0100 | 0x0200);
            if (out_b != expected) {
                fprintf(stderr, "input_profile_selftest: trigger sample %d failed: got 0x%04x expected 0x%04x\n",
                        trig_samples[i], out_b, expected);
                ok = 0;
            }
        }

        static const int16_t axis_samples[] = {
            -32768, -32767, -20000, -10000, -7850, -7849, -7848,
            -100, 0, 100, 7848, 7849, 7850, 10000, 20000, 32767
        };
        for (size_t i = 0; i < sizeof(axis_samples) / sizeof(axis_samples[0]); i++) {
            for (size_t j = 0; j < sizeof(axis_samples) / sizeof(axis_samples[0]); j++) {
                int ax = axis_samples[i];
                int ay = axis_samples[j];
                uint8_t old_lx = 128, old_ly = 128;
                if (ax < -7849 || ax > 7849) old_lx = (uint8_t)((ax + 32768) * 255 / 65535);
                if (ay < -7849 || ay > 7849) old_ly = (uint8_t)((ay + 32768) * 255 / 65535);

                bool btns[NK_HOST_BUTTON_COUNT] = {false};
                int16_t axes[NK_HOST_AXIS_COUNT] = {0};
                axes[NK_HOST_AXIS_LEFTX] = (int16_t)ax;
                axes[NK_HOST_AXIS_LEFTY] = (int16_t)ay;
                uint8_t map_lx = 0, map_ly = 0;
                sdl3vk_map_gamepad(NULL, btns, axes, NULL, &map_lx, &map_ly);
                if (map_lx != old_lx || map_ly != old_ly) {
                    fprintf(stderr, "input_profile_selftest: axis ax=%d ay=%d mismatch: got (%u,%u) expected (%u,%u)\n",
                            ax, ay, map_lx, map_ly, old_lx, old_ly);
                    ok = 0;
                }
            }
        }
    }

    /* ---- 5(b) Custom profile: remaps button, deadzone, inversion ---- */
    {
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_NORTH;
        custom.psp_buttons[NK_PSP_BTN_TRIANGLE].primary.index = NK_HOST_BUTTON_SOUTH;
        custom.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = 15000;
        custom.axes[NK_PSP_AXIS_ANALOG_X].inverted = true;

        bool btns[NK_HOST_BUTTON_COUNT] = {false};
        int16_t axes[NK_HOST_AXIS_COUNT] = {0};
        btns[NK_HOST_BUTTON_SOUTH] = true;
        uint32_t out_b = 0;
        uint8_t out_lx = 0, out_ly = 0;
        sdl3vk_map_gamepad(&custom, btns, axes, &out_b, &out_lx, &out_ly);
        if (out_b != 0x1000) {
            fprintf(stderr, "input_profile_selftest: custom remap failed: got 0x%04x expected 0x1000\n", out_b);
            ok = 0;
        }

        axes[NK_HOST_AXIS_LEFTX] = 10000;
        sdl3vk_map_gamepad(&custom, btns, axes, NULL, &out_lx, &out_ly);
        if (out_lx != 128) {
            fprintf(stderr, "input_profile_selftest: custom deadzone failed: got %u expected 128\n", out_lx);
            ok = 0;
        }

        axes[NK_HOST_AXIS_LEFTX] = 20000;
        sdl3vk_map_gamepad(&custom, btns, axes, NULL, &out_lx, &out_ly);
        uint8_t expected_inv = nk_input_profile_transform_axis(20000, 15000, 0, true);
        if (out_lx != expected_inv || out_lx >= 128) {
            fprintf(stderr, "input_profile_selftest: custom inverted axis failed: got %u expected %u (< 128)\n",
                    out_lx, expected_inv);
            ok = 0;
        }
    }

    /* ---- 5(c) Invalid profile fallback + diagnostic; missing file fallback ---- */
    {
        const char *tmp_invalid = "selftest_invalid_profile.json";
        FILE *f = fopen(tmp_invalid, "w");
        if (f) {
            fputs("{\n  \"schema_version\": 999,\n  \"deadzone_inner\": -100\n}\n", f);
            fclose(f);
        }

        profile_test_setenv("NK_INPUT_PROFILE", tmp_invalid);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();

        const NkInputProfile *prof = sdl3vk_get_input_profile();
        if (!prof || prof->schema_version != NK_INPUT_PROFILE_SCHEMA_VERSION ||
            prof->axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner != NK_INPUT_DEFAULT_DEADZONE_INNER) {
            fprintf(stderr, "input_profile_selftest: invalid profile did not fall back to defaults\n");
            ok = 0;
        }
        remove(tmp_invalid);

        profile_test_setenv("NK_INPUT_PROFILE", "selftest_nonexistent_profile_12345.json");
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        prof = sdl3vk_get_input_profile();
        if (!prof || prof->schema_version != NK_INPUT_PROFILE_SCHEMA_VERSION) {
            fprintf(stderr, "input_profile_selftest: missing profile file did not give defaults\n");
            ok = 0;
        }

        profile_test_setenv("NK_INPUT_PROFILE", NULL);
        sdl3vk_reset_input_profile();
    }

    /* ---- 5(d) SR_PADSCRIPT still wins ---- */
    {
        const char *tmp_custom = "selftest_custom_profile.json";
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_NORTH;
        custom.psp_buttons[NK_PSP_BTN_TRIANGLE].primary.index = NK_HOST_BUTTON_SOUTH;
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
        nk_input_profile_save(&custom, tmp_custom, diag, sizeof(diag));

        profile_test_setenv("NK_INPUT_PROFILE", tmp_custom);
        profile_test_setenv("SR_PADSCRIPT", "synthetic_script_active");
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();

        uint32_t south_mapped = gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH);
        if (south_mapped != 0x4000) {
            fprintf(stderr, "input_profile_selftest: SR_PADSCRIPT active did not prioritize defaults: got 0x%04x expected 0x4000\n",
                    south_mapped);
            ok = 0;
        }

        profile_test_setenv("SR_PADSCRIPT", NULL);
        profile_test_setenv("NK_INPUT_PROFILE", NULL);
        remove(tmp_custom);
        sdl3vk_reset_input_profile();
    }

    /* ---- 5(e) Keyboard keys come from the profile (#1299) ---- */
    {
        /* The default profile reproduces the map hard-coded before schema 3, key name and
         * SDL scancode alike. */
        static const struct {
            SDL_Scancode sc;
            const char *name;
            uint32_t expected_bit;
        } kKeyTable[] = {
            { SDL_SCANCODE_RETURN, "return", 0x0008 }, /* START */
            { SDL_SCANCODE_LSHIFT, "lshift", 0x0001 }, /* SELECT */
            { SDL_SCANCODE_RSHIFT, "rshift", 0x0001 }, /* SELECT */
            { SDL_SCANCODE_X,      "x",      0x4000 }, /* CROSS */
            { SDL_SCANCODE_Z,      "z",      0x2000 }, /* CIRCLE */
            { SDL_SCANCODE_A,      "a",      0x8000 }, /* SQUARE */
            { SDL_SCANCODE_S,      "s",      0x1000 }, /* TRIANGLE */
            { SDL_SCANCODE_Q,      "q",      0x0100 }, /* LTRIGGER */
            { SDL_SCANCODE_W,      "w",      0x0200 }, /* RTRIGGER */
            { SDL_SCANCODE_UP,     "up",     0x0010 },
            { SDL_SCANCODE_DOWN,   "down",   0x0040 },
            { SDL_SCANCODE_LEFT,   "left",   0x0080 },
            { SDL_SCANCODE_RIGHT,  "right",  0x0020 },
        };
        for (size_t i = 0; i < sizeof(kKeyTable) / sizeof(kKeyTable[0]); i++) {
            bool keys[SDL_SCANCODE_COUNT] = {false};
            keys[kKeyTable[i].sc] = true;
            uint32_t held = sdl3vk_map_keyboard(NULL, keys, SDL_SCANCODE_COUNT);
            uint32_t pressed = sdl3vk_map_key(NULL, (int)kKeyTable[i].sc);
            if (held != kKeyTable[i].expected_bit || pressed != kKeyTable[i].expected_bit) {
                fprintf(stderr, "input_profile_selftest: default key %s: held 0x%04x pressed 0x%04x "
                        "expected 0x%04x\n", kKeyTable[i].name, held, pressed,
                        kKeyTable[i].expected_bit);
                ok = 0;
            }
            if (nk_host_key_from_name(kKeyTable[i].name) != (int)kKeyTable[i].sc) {
                fprintf(stderr, "input_profile_selftest: key name '%s' is not SDL scancode %d\n",
                        kKeyTable[i].name, (int)kKeyTable[i].sc);
                ok = 0;
            }
        }
        {
            bool keys[SDL_SCANCODE_COUNT] = {false};
            keys[SDL_SCANCODE_C] = true;
            if (sdl3vk_map_keyboard(NULL, keys, SDL_SCANCODE_COUNT) != 0 ||
                sdl3vk_map_key(NULL, SDL_SCANCODE_C) != 0) {
                fprintf(stderr, "input_profile_selftest: unbound default key C produced a button\n");
                ok = 0;
            }
            /* A key past the host's reported state array reads as released. */
            keys[SDL_SCANCODE_X] = true;
            if (sdl3vk_map_keyboard(NULL, keys, SDL_SCANCODE_X) != 0) {
                fprintf(stderr, "input_profile_selftest: key beyond key_count read as held\n");
                ok = 0;
            }
        }

        /* A saved profile that moves CROSS from X to C: the runtime loads it and the
         * production key paths set CROSS for C and nothing for X. */
        const char *tmp_keys = "selftest_keyboard_profile.json";
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_keys[NK_PSP_BTN_CROSS].primary = nk_host_key_from_name("c");
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
        if (nk_input_profile_save(&custom, tmp_keys, diag, sizeof(diag)) != NK_OK) {
            fprintf(stderr, "input_profile_selftest: keyboard profile save failed: %s\n", diag);
            ok = 0;
        }
        profile_test_setenv("NK_INPUT_PROFILE", tmp_keys);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        const NkInputProfile *prof = sdl3vk_get_input_profile();
        bool keys[SDL_SCANCODE_COUNT] = {false};
        keys[SDL_SCANCODE_C] = true;
        uint32_t held_c = sdl3vk_map_keyboard(prof, keys, SDL_SCANCODE_COUNT);
        keys[SDL_SCANCODE_C] = false;
        keys[SDL_SCANCODE_X] = true;
        uint32_t held_x = sdl3vk_map_keyboard(prof, keys, SDL_SCANCODE_COUNT);
        uint32_t pulse_c = keyboard_button(SDL_SCANCODE_C);
        uint32_t pulse_x = keyboard_button(SDL_SCANCODE_X);
        if (held_c != NK_PSP_BTN_CROSS_BIT || pulse_c != NK_PSP_BTN_CROSS_BIT ||
            held_x != 0 || pulse_x != 0) {
            fprintf(stderr, "input_profile_selftest: remapped key: C held 0x%04x pressed 0x%04x, "
                    "X held 0x%04x pressed 0x%04x; expected C -> 0x4000 and X -> 0\n",
                    held_c, pulse_c, held_x, pulse_x);
            ok = 0;
        }

        /* SR_PADSCRIPT keeps the default keys even with the remap on disk. */
        profile_test_setenv("SR_PADSCRIPT", "synthetic_script_active");
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        if (keyboard_button(SDL_SCANCODE_X) != NK_PSP_BTN_CROSS_BIT ||
            keyboard_button(SDL_SCANCODE_C) != 0) {
            fprintf(stderr, "input_profile_selftest: SR_PADSCRIPT active did not keep default keys\n");
            ok = 0;
        }

        profile_test_setenv("SR_PADSCRIPT", NULL);
        profile_test_setenv("NK_INPUT_PROFILE", NULL);
        remove(tmp_keys);
        sdl3vk_reset_input_profile();
    }

    return ok;
}

/* ---- pause input selftest (#1159); no Vulkan needed ------------------------------------
 * Drives the real event path (poll_input) with an SDL virtual gamepad and pushed key events:
 * Esc, F1 and Guide ask for the pause and never reach the game; while the overlay holds the
 * claim every event goes to it and the game reads a released pad; a button held when the
 * claim ends is not a fresh press until it is let go (P8); a stick pushed when the claim ends
 * reads centred until it returns; and losing the open pad asks for the pause. */

typedef struct {
    int events;
    int south_downs;
    int guide_downs;
    int escapes;
} PauseTestSink;

static void pause_test_sink(const void *event, void *user) {
    const SDL_Event *ev = (const SDL_Event *)event;
    PauseTestSink *sink = (PauseTestSink *)user;
    sink->events++;
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
        sink->south_downs++;
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
        sink->guide_downs++;
    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE)
        sink->escapes++;
}

static void pause_test_key(SDL_Keycode key, bool repeat) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = key;
    ev.key.down = true;
    ev.key.repeat = repeat;
    SDL_PushEvent(&ev);
}

/* SDL holds back a Guide release that comes within its minimum Guide press time (250 ms)
 * and drops a new Guide press until that release is out, so a test tap waits it out. */
static void pause_test_guide_release(SDL_Joystick *joy) {
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, false);
    SDL_Delay(300);
    int quit = 0;
    poll_input(&quit);
    sdl3vk_consume_button_pulses();
}

/* One host poll followed by one game sample, the order a VBLANK sees them in. */
static uint32_t pause_test_sample(void) {
    int quit = 0;
    poll_input(&quit);
    uint32_t seen = sdl3vk_buttons();
    sdl3vk_consume_button_pulses();
    return seen;
}

#define PAUSE_CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "pause_input_selftest: " __VA_ARGS__); fputc('\n', stderr); ok = 0; } } while (0)

int sdl3vk_pause_input_selftest(void) {
    int ok = 1;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "pause_input_selftest: SDL gamepad subsystem unavailable: %s\n", SDL_GetError());
        return 0;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1u;
    desc.name = "Nakagawa pause selftest pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick *joy = id ? SDL_OpenJoystick(id) : NULL;
    if (!joy) {
        fprintf(stderr, "pause_input_selftest: virtual gamepad unavailable: %s\n", SDL_GetError());
        if (id) SDL_DetachVirtualJoystick(id);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return 0;
    }
    /* Physical button i is SDL_GamepadButton i under this mask. */
    const uint32_t cross = gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH);
    const uint32_t circle = gamepad_button(SDL_GAMEPAD_BUTTON_EAST);
    PauseTestSink sink = {0};

    (void)pause_test_sample();
    PAUSE_CHECK(s_pad != NULL, "the virtual gamepad was not opened");
    PAUSE_CHECK(cross && circle && cross != circle, "default mapping lacks cross/circle");
    (void)sdl3vk_take_pause_request();
    (void)sdl3vk_take_pad_pulses();

    /* The game sees a press, and Guide is the pause button only. */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    PAUSE_CHECK(pause_test_sample() & cross, "cross press not seen by the game");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    (void)pause_test_sample();
    (void)sdl3vk_take_pad_pulses();
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, true);
    PAUSE_CHECK(pause_test_sample() == 0, "Guide reached the game");
    PAUSE_CHECK(sdl3vk_take_pad_pulses() == 0, "Guide reached the keyboard's pad pulses");
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER, "Guide did not ask for the pause");
    pause_test_guide_release(joy);

    /* A profile may bind Guide (HOME is the documented choice); Guide is still the pause
     * button and the binding never fires in game. */
    {
        const char *tmp_profile = "selftest_pause_guide_profile.json";
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_buttons[NK_PSP_BTN_HOME].primary.type = NK_BINDING_HOST_BUTTON;
        custom.psp_buttons[NK_PSP_BTN_HOME].primary.index = NK_HOST_BUTTON_GUIDE;
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
        PAUSE_CHECK(nk_input_profile_save(&custom, tmp_profile, diag, sizeof(diag)) == NK_OK,
                    "profile save failed: %s", diag);
        profile_test_setenv("NK_INPUT_PROFILE", tmp_profile);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, true);
        (void)pause_test_sample();
        PAUSE_CHECK((pause_test_sample() & NK_PSP_BTN_HOME_BIT) == 0, "a Guide binding reached the game");
        PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER,
                    "Guide bound in the profile did not ask for the pause");
        pause_test_guide_release(joy);
        profile_test_setenv("NK_INPUT_PROFILE", NULL);
        remove(tmp_profile);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
    }

    /* Esc and F1 ask; a held key's repeats do not ask again. */
    pause_test_key(SDLK_ESCAPE, false);
    (void)pause_test_sample();
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER, "Esc did not ask for the pause");
    PAUSE_CHECK(s_key_count == 0, "Esc reached the key queue");
    pause_test_key(SDLK_F1, false);
    (void)pause_test_sample();
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER, "F1 did not ask for the pause");
    pause_test_key(SDLK_ESCAPE, true);
    (void)pause_test_sample();
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_NONE, "a key repeat asked again");

    /* The overlay holds the claim: everything is its own. */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    (void)pause_test_sample();        /* cross held before the pause opens */
    sdl3vk_set_input_claim(pause_test_sink, &sink);
    PAUSE_CHECK(sdl3vk_input_claimed(), "the claim did not take");
    PAUSE_CHECK(sdl3vk_buttons() == 0, "a held button shows through the claim");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    (void)pause_test_sample();
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    PAUSE_CHECK(pause_test_sample() == 0, "a press reached the game through the claim");
    PAUSE_CHECK(sdl3vk_take_pad_pulses() == 0, "a press reached the pad pulses through the claim");
    PAUSE_CHECK(sink.south_downs == 1, "the overlay saw %d cross presses, expected 1", sink.south_downs);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, true);
    pause_test_key(SDLK_ESCAPE, false);
    (void)pause_test_sample();
    pause_test_guide_release(joy);
    PAUSE_CHECK(sink.guide_downs == 1 && sink.escapes == 1, "the overlay saw Guide %d times and Esc %d times, expected 1 and 1", sink.guide_downs, sink.escapes);
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_NONE, "a request latched under the claim");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
    (void)pause_test_sample();
    uint8_t lx = 0, ly = 0;
    sdl3vk_analog(&lx, &ly);
    PAUSE_CHECK(lx == 128 && ly == 128, "the stick reached the game through the claim");

    /* P8: cross is still held as the overlay closes. */
    sdl3vk_set_input_claim(NULL, NULL);
    PAUSE_CHECK(!sdl3vk_input_claimed(), "the claim did not end");
    PAUSE_CHECK((sdl3vk_buttons() & cross) == 0, "the held cross shows at release");
    for (int i = 0; i < 3; i++)
        PAUSE_CHECK((pause_test_sample() & cross) == 0, "the held cross became a press (sample %d)", i);
    sdl3vk_analog(&lx, &ly);
    PAUSE_CHECK(lx == 128, "the pushed stick shows before it returns");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, true);
    PAUSE_CHECK(pause_test_sample() & circle, "a button pressed after the release is not seen");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    PAUSE_CHECK((pause_test_sample() & (cross | circle)) == 0, "released buttons still show");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    PAUSE_CHECK(pause_test_sample() & cross, "cross pressed again after its release is not seen");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    (void)pause_test_sample();
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    (void)pause_test_sample();
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
    (void)pause_test_sample();
    sdl3vk_analog(&lx, &ly);
    PAUSE_CHECK(lx > 128, "the stick pushed again after centring is not seen");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    (void)pause_test_sample();
    (void)sdl3vk_take_pad_pulses();

    /* Losing the open pad asks for the pause. */
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    (void)pause_test_sample();
    PAUSE_CHECK(s_pad == NULL, "the removed pad stayed open");
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_PAD_LOST, "losing the pad did not ask for the pause");

    if (s_pad) { SDL_CloseGamepad(s_pad); s_pad = NULL; }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    nk_input_gate_init(&s_gate);
    s_pause_request = SDL3VK_PAUSE_REQUEST_NONE;
    s_button_pulse = s_pad_pulse = 0;
    s_buttons = 0;
    s_lx = s_ly = 128;
    s_pad_present = 0;
    if (ok) printf("pause_input_selftest: OK\n");
    return ok;
}
#undef PAUSE_CHECK

/* Set while the lifecycle selftest holds its own SDL video reference: SDL_WasInit cannot tell
 * that reference from one sdl3vk leaked, so the per-step count then checks the gamepad
 * subsystem only, and the selftest checks video once after releasing its reference. */
static int s_lifecycle_test_holds_video = 0;

/* Objects sdl3vk_init can create, and its SDL subsystems, that are still held. */
static int lifecycle_live_objects(void) {
    static const PresentFrame empty;
    int n = (s_win != NULL) + (s_pad != NULL) + (s_inst != VK_NULL_HANDLE) +
            (s_validation_msgr != VK_NULL_HANDLE) + (s_surf != VK_NULL_HANDLE) +
            (s_dev != VK_NULL_HANDLE) + (s_pool != VK_NULL_HANDLE) + (s_cmd != VK_NULL_HANDLE) +
            (s_fence != VK_NULL_HANDLE) + (s_swap != VK_NULL_HANDLE) + s_sdl_subsystems;
    for (int i = 0; i < PRESENT_FRAMES; i++)
        n += memcmp(&s_frame[i], &empty, sizeof empty) != 0;
    for (size_t i = 0; i < sizeof(s_swap_sem_done) / sizeof(s_swap_sem_done[0]); i++)
        n += s_swap_sem_done[i] != VK_NULL_HANDLE;
    return n + (SDL_WasInit(s_lifecycle_test_holds_video ? SDL_INIT_GAMEPAD
                                                         : SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) != 0);
}

/* Init unwind, repeated shutdown and re-init (#1377). A failure injected at each fallible step
 * of sdl3vk_init in turn must leave no Vulkan object, window or SDL subsystem alive and raise no
 * validation error (with validation on, vkDestroyDevice and vkDestroyInstance report any child
 * the unwind missed). Returns 0, 77 when Vulkan is unavailable, or 1. */
static int sdl3vk_lifecycle_selftest(void) {
    if (!sdl3vk_init("Nakagawa GPU lifecycle selftest")) {
        fprintf(stderr, "gpu capture selftest: SKIP (Vulkan initialization unavailable)\n");
        return 77;
    }
    const int steps = s_init_steps;
    const int errors = sdl3vk_validation_error_count();
    int ok = 1;
    if (!s_validation_on) {
        fprintf(stderr, "lifecycle: SR_VULKAN_VALIDATION=1 did not reach sdl3vk_init; "
                        "the validation layer is off\n");
        ok = 0;
    }
    /* The test's own instance keeps the driver loaded between the cycles below: the NVIDIA
     * Windows driver was seen to fault in a worker thread of its own when it is unloaded and
     * loaded again many times in quick succession. It is not one of sdl3vk's objects. */
    VkInstanceCreateInfo keep_ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance keep = VK_NULL_HANDLE;
    (void)vkCreateInstance(&keep_ci, NULL, &keep);
    /* The test holds its own SDL video reference across the cycles too. sdl3vk_shutdown
     * releases only its own (SDL_QuitSubSystem), so the video backend's display connection
     * stays open instead of being reopened some 70 times: under xvfb on a loaded CI runner
     * one reconnect was seen to fail ("x11 not available"), which the loop then reported as
     * an unwind at the wrong step. sdl3vk's own objects are still counted below; a video
     * reference it leaked in any cycle is caught once the test releases its own. */
    const int keep_video = SDL_InitSubSystem(SDL_INIT_VIDEO) ? 1 : 0;
    s_lifecycle_test_holds_video = keep_video;
    sdl3vk_shutdown();
    sdl3vk_shutdown();
    if (lifecycle_live_objects() != 0) {
        fprintf(stderr, "lifecycle: %d object(s) alive after shutdown\n", lifecycle_live_objects());
        ok = 0;
    }
    for (int step = 1; ok && step <= steps; step++) {
        s_init_fault_step = step;
        int inited = sdl3vk_init("Nakagawa GPU lifecycle selftest");
        s_init_fault_step = 0;
        int live = lifecycle_live_objects();
        if (inited || s_init_steps != step || live != 0) {
            fprintf(stderr, "lifecycle: init step %d of %d failed (%s) and left %d object(s) alive\n",
                    step, steps, inited || s_init_steps != step ? "not where injected" : "as injected",
                    live);
            ok = 0;
        }
    }
    /* A failure at each checked step of a present (#1377) is reported; the renderer is
     * terminal exactly when the step leaves a frame, or an acquired image, that cannot be
     * handed back; a terminal renderer refuses the next present, a recovered one presents it;
     * and shutdown and re-init leave no object alive and raise no validation error. A
     * suboptimal acquire is presented, not rebuilt over with its image still acquired. */
    static const struct {
        const char *name;
        int step;
        VkResult result;
        int want, terminal;     /* the faulted present's result; the renderer state after it */
    } pcases[] = {
        { "frame fence wait",       PSTEP_FRAME_WAIT,  VK_ERROR_DEVICE_LOST,          -1, 1 },
        { "acquire, out of memory", PSTEP_ACQUIRE,     VK_ERROR_OUT_OF_HOST_MEMORY,   -1, 0 },
        { "acquire, device lost",   PSTEP_ACQUIRE,     VK_ERROR_DEVICE_LOST,          -1, 1 },
        { "acquire, suboptimal",    PSTEP_ACQUIRE,     VK_SUBOPTIMAL_KHR,              1, 0 },
        { "image fence wait",       PSTEP_IMAGE_WAIT,  VK_ERROR_DEVICE_LOST,          -1, 1 },
        { "command buffer reset",   PSTEP_RESET_CMD,   VK_ERROR_OUT_OF_DEVICE_MEMORY, -1, 1 },
        { "command buffer begin",   PSTEP_BEGIN_CMD,   VK_ERROR_OUT_OF_HOST_MEMORY,   -1, 1 },
        { "command buffer end",     PSTEP_END_CMD,     VK_ERROR_OUT_OF_DEVICE_MEMORY, -1, 1 },
        { "fence reset",            PSTEP_RESET_FENCE, VK_ERROR_OUT_OF_DEVICE_MEMORY, -1, 1 },
        { "submit",                 PSTEP_SUBMIT,      VK_ERROR_DEVICE_LOST,          -1, 1 },
    };
    static uint32_t px[PSP_W * PSP_H];
    const int npcases = (int)(sizeof pcases / sizeof pcases[0]);
    for (int i = 0; ok && i < npcases; i++) {
        if (!sdl3vk_init("Nakagawa GPU lifecycle selftest")) {
            fprintf(stderr, "lifecycle: re-init before present case '%s' failed\n", pcases[i].name);
            ok = 0;
            break;
        }
        /* Every frame submitted once, so the next present waits on its frame's fence. */
        int warm = 1;
        for (int k = 0; k < PRESENT_FRAMES; k++) warm &= sdl3vk_present_rgba(px) == 1;
        /* The image fence wait runs only when the acquired image was last drawn by the other
         * frame, which a swapchain whose image count is a multiple of PRESENT_FRAMES never
         * does: mark every image as drawn by the other frame (its fence was submitted above). */
        if (pcases[i].step == PSTEP_IMAGE_WAIT)
            for (uint32_t k = 0; k < s_swap_n; k++)
                s_swap_img_fence[k] = s_frame[(s_frame_cursor + 1) % PRESENT_FRAMES].fence;
        s_pstep_fault = pcases[i].step;
        s_pstep_fault_result = pcases[i].result;
        int got = 0, presents = 0;
        while (s_pstep_fault != PSTEP_NONE && presents < 8) {
            got = sdl3vk_present_rgba(px);
            presents++;
        }
        int reached = s_pstep_fault == PSTEP_NONE;
        s_pstep_fault = PSTEP_NONE;
        int terminal = sdl3vk_renderer_terminal();
        int after = sdl3vk_present_rgba(px);
        sdl3vk_shutdown();
        int live = lifecycle_live_objects();
        if (!warm || !reached || got != pcases[i].want || terminal != pcases[i].terminal ||
            after != (pcases[i].terminal ? -1 : 1) || live != 0) {
            fprintf(stderr, "lifecycle: present fault '%s': warm=%d reached=%d present=%d (want %d) "
                            "terminal=%d (want %d) next present=%d, %d object(s) alive after "
                            "shutdown\n", pcases[i].name, warm, reached, got, pcases[i].want,
                    terminal, pcases[i].terminal, after, live);
            ok = 0;
        }
    }
    if (keep) vkDestroyInstance(keep, NULL);
    if (keep_video) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        s_lifecycle_test_holds_video = 0;
        if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "lifecycle: SDL video still initialised after the cycles "
                            "(a reference sdl3vk_init took was not released)\n");
            ok = 0;
        }
    }
    if (sdl3vk_validation_error_count() != errors) {
        fprintf(stderr, "lifecycle: %d validation error(s) during the unwinds\n",
                sdl3vk_validation_error_count() - errors);
        ok = 0;
    }
    if (!ok) return 1;
    printf("gpu lifecycle selftest: init unwind at each of %d steps OK; %d present faults OK\n",
           steps, npcases);
    return 0;
}

static int sdl3vk_capture_selftest_body(void) {
    int ok = 1;

    /* ---- pure framebuffer-capture policy (fbcap_policy.h); no Vulkan needed --------- */
    if (sr_fbcap_owner(0, 0) != SR_FBCAP_NONE) {
        fprintf(stderr, "policy: nothing due must not own the slot\n"); ok = 0;
    }
    if (sr_fbcap_owner(1, 1) != SR_FBCAP_FBDUMP ||
        sr_fbcap_owner(1, 0) != SR_FBCAP_FBDUMP) {
        fprintf(stderr, "policy: SR_FBDUMP must outrank SR_FBSNAP\n"); ok = 0;
    }
    if (sr_fbcap_owner(0, 1) != SR_FBCAP_FBSNAP) {
        fprintf(stderr, "policy: SR_FBSNAP due must own the slot\n"); ok = 0;
    }
    {
        char p[128];
        if (!sr_fbcap_path(SR_FBCAP_FBSNAP, 12, p, sizeof p) ||
            strcmp(p, "build/snapshots/frame_0012.ppm") != 0) {
            fprintf(stderr, "policy: FBSNAP path wrong: '%s'\n", p); ok = 0;
        }
        if (!sr_fbcap_path(SR_FBCAP_FBDUMP, 0, p, sizeof p) ||
            strcmp(p, "present_source.ppm") != 0) {
            fprintf(stderr, "policy: FBDUMP path wrong: '%s'\n", p); ok = 0;
        }
        if (sr_fbcap_path(SR_FBCAP_NONE, 0, p, sizeof p) || p[0] != '\0') {
            fprintf(stderr, "policy: NONE owner must not produce a path\n"); ok = 0;
        }
    }
    if (sr_fbcap_exit_status(SR_FBCAP_NONE, 1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_NONE, -1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, 1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, 0) != 1 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, -1) != 1) {
        fprintf(stderr, "policy: exit-status table wrong\n"); ok = 0;
    }
    if (!ok) return 1;

    /* ---- readback memory type (#1296); no Vulkan needed ----------------------------- */
    /* An RTX 3080's types: 3 is visible+coherent but uncached, 4 adds HOST_CACHED. A capture
     * or probe buffer the CPU reads must take a cached type when one is allowed: the first
     * visible+coherent type made a full read of a 4x frame cost about a second. */
    {
        const uint32_t vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        const uint32_t coh = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const uint32_t cac = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        const uint32_t dev = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const uint32_t rtx3080[6] = { 0u, dev, dev, vis | coh, vis | coh | cac, dev | vis | coh };
        const uint32_t cached_noncoherent[3] = { vis | coh, vis | cac, 0u };
        const uint32_t noncoherent_only[2] = { dev, vis };
        int nc = -1;
        if (cap_pick_mem_type(rtx3080, 6u, 0x3fu, &nc) != 4u || nc != 0) {
            fprintf(stderr, "readback memory: the cached+coherent type must win\n"); ok = 0;
        }
        if (cap_pick_mem_type(rtx3080, 6u, 0x2fu, &nc) != 3u || nc != 0) {
            fprintf(stderr, "readback memory: without a cached type, the first visible+coherent\n");
            ok = 0;
        }
        if (cap_pick_mem_type(cached_noncoherent, 3u, 0x7u, &nc) != 1u || nc != 1) {
            fprintf(stderr, "readback memory: a cached non-coherent type beats an uncached "
                            "coherent one, and must be invalidated\n");
            ok = 0;
        }
        if (cap_pick_mem_type(noncoherent_only, 2u, 0x3u, &nc) != 1u || nc != 1) {
            fprintf(stderr, "readback memory: the last resort is any host-visible type\n"); ok = 0;
        }
        if (cap_pick_mem_type(rtx3080, 6u, 0x07u, &nc) != UINT32_MAX) {
            fprintf(stderr, "readback memory: no host-visible type allowed must be refused\n");
            ok = 0;
        }
    }
    if (!ok) return 1;

    /* ---- input profile mapping selftest (#357); no Vulkan needed ------------------- */
    if (!sdl3vk_input_profile_selftest()) {
        fprintf(stderr, "gpu capture selftest: input profile selftest failed\n");
        return 1;
    }

    /* ---- pause input claim and release mask (#1159); no Vulkan needed -------------- */
    if (!sdl3vk_pause_input_selftest()) {
        fprintf(stderr, "gpu capture selftest: pause input selftest failed\n");
        return 1;
    }

    /* ---- validation-layer requirement ---------------------------------------------- */
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties lp[64];
        int have = 0;
        if (n > 64) n = 64;
        if (vkEnumerateInstanceLayerProperties(&n, lp) == VK_SUCCESS)
            for (uint32_t i = 0; i < n; i++)
                if (strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) have = 1;
        if (!have) {
            fprintf(stderr, "gpu capture selftest: SKIP (VK_LAYER_KHRONOS_validation "
                            "not installed)\n");
            return 77;
        }
    }
    if (!cap_test_setenv("SR_VULKAN_VALIDATION", "1")) {
        fprintf(stderr, "gpu capture selftest: cannot set SR_VULKAN_VALIDATION\n");
        return 1;
    }
    const char *terminal_fault = getenv("SR_GPU_CAPTURE_TERMINAL_CASE");
    if (!terminal_fault || !terminal_fault[0]) {
        int lifecycle = sdl3vk_lifecycle_selftest();
        if (lifecycle != 0) return lifecycle;
    }
    if (!sdl3vk_init("Nakagawa GPU capture selftest")) {
        fprintf(stderr, "gpu capture selftest: SKIP (Vulkan initialization unavailable)\n");
        return 77;
    }
    /* Every "no validation error" check below means something only if the layer is on. */
    if (!s_validation_on) {
        fprintf(stderr, "gpu capture selftest: SR_VULKAN_VALIDATION=1 did not reach sdl3vk_init; "
                        "the validation layer is off\n");
        sdl3vk_shutdown();
        return 1;
    }

    /* ---- filter selection policy (issue #68) --------------------------------------- */
    /* Exact integer scaling must select VK_FILTER_NEAREST */
    if (sdl3vk_present_filter(480, 272, 960, 544) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 960x544 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(960, 544, 960, 544) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 960x544 -> 960x544 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 480, 272) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 480x272 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1440, 816) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 1440x816 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1920, 1088) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 1920x1088 must select NEAREST\n"); ok = 0;
    }
    /* Fractional and non-integer scaling must select VK_FILTER_LINEAR */
    if (sdl3vk_present_filter(480, 272, 720, 408) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 720x408 (1.5x) must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1200, 680) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 1200x680 (2.5x) must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1000, 566) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 1000x566 fractional must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(960, 544, 1440, 816) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 960x544 -> 1440x816 fractional must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 960, 540) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 960x540 non-uniform must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(0, 0, 960, 544) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: invalid dimensions must select LINEAR\n"); ok = 0;
    }

    /* ---- CPU framebuffer path (B8G8R8A8 fbimg) -------------------------------------- */
    static uint32_t px[PSP_W * PSP_H];
    cap_test_fill_cpu(px, PSP_W, PSP_H);
    if (terminal_fault && terminal_fault[0]) {
        VkResult fault_result;
        if (strcmp(terminal_fault, "surface-lost") == 0) {
            fault_result = VK_ERROR_SURFACE_LOST_KHR;
        } else if (strcmp(terminal_fault, "unclassified") == 0) {
            fault_result = (VkResult)-9999;
        } else if (strcmp(terminal_fault, "device-lost") == 0) {
            fault_result = VK_ERROR_DEVICE_LOST;
        } else {
            fprintf(stderr, "gpu capture selftest: unknown terminal fault case '%s'\n",
                    terminal_fault);
            ok = 0;
            fault_result = VK_SUCCESS;
        }
        if (ok) {
            uint64_t sem0 = sdl3vk_frame_semaphore_generation();
            if (!sr_capture_arm("selftest_terminal.ppm")) {
                fprintf(stderr, "terminal fault %s: capture arm failed\n", terminal_fault);
                ok = 0;
            } else {
                sdl3vk_present_fault_inject((int)fault_result);
                if (sdl3vk_present_rgba(px) != -1 || sr_capture_result() != -1) {
                    fprintf(stderr, "terminal fault %s: expected failed present and capture\n",
                            terminal_fault);
                    ok = 0;
                }
                if (!sdl3vk_renderer_terminal()) {
                    fprintf(stderr, "terminal fault %s: renderer did not enter terminal state\n",
                            terminal_fault);
                    ok = 0;
                }
                if (sdl3vk_frame_semaphore_generation() != sem0) {
                    fprintf(stderr, "terminal fault %s: semaphore generation changed\n",
                            terminal_fault);
                    ok = 0;
                }
            }
        }
        if (sdl3vk_validation_error_count() != 0) {
            fprintf(stderr, "terminal fault %s: validation reported errors\n", terminal_fault);
            ok = 0;
        }
        sdl3vk_shutdown();
        if (sdl3vk_validation_error_count() != 0) {
            fprintf(stderr, "terminal fault %s: validation reported teardown errors\n",
                    terminal_fault);
            ok = 0;
        }
        if (!ok) return 1;
        printf("gpu capture selftest: terminal fault %s OK\n", terminal_fault);
        return 0;
    }
    if (!sr_capture_arm("selftest_cpu.ppm")) {
        fprintf(stderr, "cpu: capture arm refused\n"); ok = 0;
    }
    if (sdl3vk_present_rgba(px) != 1) {
        fprintf(stderr, "cpu: present failed\n"); ok = 0;
    }
    if (sr_capture_result() != 1) {
        fprintf(stderr, "cpu: capture result %d (expected 1)\n", sr_capture_result());
        ok = 0;
    }
    if (strcmp(sr_capture_source_label(), "cpu-framebuffer") != 0) {
        fprintf(stderr, "cpu: source label '%s'\n", sr_capture_source_label());
        ok = 0;
    }
    if (!cap_test_verify_ppm("selftest_cpu.ppm", PSP_W, PSP_H)) {
        fprintf(stderr, "cpu: PPM bytes wrong (header, channel order, pitch, or truncation)\n");
        ok = 0;
    }
    remove("selftest_cpu.ppm");

    /* ---- pause overlay (#1159): shown, repeated without an upload, never captured ------ */
    {
        int ow = 0, oh = 0, orgba = 0;
        if (!sdl3vk_overlay_extent(&ow, &oh, &orgba)) {
            fprintf(stderr, "overlay: no extent for a live swapchain\n"); ok = 0;
        } else {
            const size_t opitch = (size_t)ow * 4u;
            uint8_t *opx = (uint8_t *)malloc(opitch * (size_t)oh);
            if (!opx) {
                fprintf(stderr, "overlay: out of memory\n"); ok = 0;
            } else {
                for (size_t i = 0; i < opitch * (size_t)oh; i++) opx[i] = (uint8_t)(i * 7u);
                if (!sr_capture_arm("selftest_overlay.ppm")) {
                    fprintf(stderr, "overlay: capture arm refused\n"); ok = 0;
                }
                int first = sdl3vk_present_overlay(NULL, 0);
                if (first != 2) {
                    fprintf(stderr, "overlay: a present before any image returned %d (expected 2)\n", first);
                    ok = 0;
                }
                int shown = sdl3vk_present_overlay(opx, (int)opitch);
                int again = sdl3vk_present_overlay(NULL, 0);
                if (shown != 1 || again != 1) {
                    fprintf(stderr, "overlay: presents returned %d, %d (expected 1, 1)\n", shown, again);
                    ok = 0;
                }
                if (sr_capture_result() != 0) {
                    fprintf(stderr, "overlay: an overlay present serviced the capture\n"); ok = 0;
                }
                sr_capture_cancel();
                FILE *f = fopen("selftest_overlay.ppm", "rb");
                if (f) { fclose(f); fprintf(stderr, "overlay: capture file must not exist\n"); ok = 0; }
                free(opx);
            }
            sdl3vk_overlay_release();
        }
    }

    /* ---- cancel: an armed capture never serviced reports "nothing attempted" --------- */
    if (!sr_capture_arm("selftest_cancel.ppm")) {
        fprintf(stderr, "cancel: arm refused\n"); ok = 0;
    }
    sr_capture_cancel();
    if (sr_capture_result() != 0) {
        fprintf(stderr, "cancel: result %d (expected 0)\n", sr_capture_result());
        ok = 0;
    }
    {
        FILE *f = fopen("selftest_cancel.ppm", "rb");
        if (f) { fclose(f); fprintf(stderr, "cancel: file must not exist\n"); ok = 0; }
    }

    /* ---- GPU render-target path (R8G8B8A8, same 480x272 region) --------------------- */
    CapTestImage gpu;
    if (!cap_test_image_create(&gpu, PSP_W, PSP_H) || !cap_test_image_upload(&gpu)) {
        fprintf(stderr, "gpu: image setup failed\n"); ok = 0;
    } else {
        if (!sr_capture_arm("selftest_gpu.ppm")) {
            fprintf(stderr, "gpu: capture arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)gpu.img, PSP_W, PSP_H) != 1) {
            fprintf(stderr, "gpu: present failed\n"); ok = 0;
        }
        if (sr_capture_result() != 1) {
            fprintf(stderr, "gpu: capture result %d (expected 1)\n", sr_capture_result());
            ok = 0;
        }
        if (strcmp(sr_capture_source_label(), "gpu-render-target") != 0) {
            fprintf(stderr, "gpu: source label '%s'\n", sr_capture_source_label());
            ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_gpu.ppm", PSP_W, PSP_H)) {
            fprintf(stderr, "gpu: PPM bytes wrong (BGRA/RGBA handling, pitch, or truncation)\n");
            ok = 0;
        }
        remove("selftest_gpu.ppm");
        cap_test_image_destroy(&gpu);
    }

    /* ---- scaled GPU path: a larger source exercises row pitch and buffer growth ------ */
    CapTestImage big;
    if (!cap_test_image_create(&big, 960, 544) || !cap_test_image_upload(&big)) {
        fprintf(stderr, "scaled: image setup failed\n"); ok = 0;
    } else {
        if (!sr_capture_arm("selftest_scaled.ppm")) {
            fprintf(stderr, "scaled: capture arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)big.img, 960, 544) != 1) {
            fprintf(stderr, "scaled: present failed\n"); ok = 0;
        }
        if (sr_capture_result() != 1) {
            fprintf(stderr, "scaled: capture result %d (expected 1)\n", sr_capture_result());
            ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_scaled.ppm", 960, 544)) {
            fprintf(stderr, "scaled: PPM bytes wrong (pitch or truncation)\n"); ok = 0;
        }
        remove("selftest_scaled.ppm");
        cap_test_image_destroy(&big);
    }

    /* ---- picture probe at a 4x render scale (#1296) ---------------------------------
     * The probe's readback is not waited for or scanned inside the present: the present
     * returns with the probe in flight and no result, a file capture can be armed and
     * published for the next present, and the probe's own pixels (not the newer frame's)
     * give the result once its fence has signalled. Its buffer comes from the same picker
     * as the readback memory cells above. */
    CapTestImage dark;
    if (!cap_test_image_create(&dark, 2048, 1088) || !cap_test_image_upload_dark(&dark, 8u)) {
        fprintf(stderr, "probe: image setup failed\n"); ok = 0;
    } else {
        uint32_t b = 0u;
        if (!sr_capture_arm_probe()) {
            fprintf(stderr, "probe: arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != 1) {
            fprintf(stderr, "probe: present failed\n"); ok = 0;
        }
        if (!sr_capture_probe_in_flight() || sr_capture_probe_take(&b)) {
            fprintf(stderr, "probe: the present must return with the probe in flight and no "
                            "result (no fence wait or scan inside the present)\n");
            ok = 0;
        }
        if (sr_capture_arm_probe()) {
            fprintf(stderr, "probe: a second probe must be refused while one is in flight\n");
            ok = 0;
            sr_capture_cancel();
        }
        if (!s_probe.buf || s_probe.buf == s_cap.buf) {
            fprintf(stderr, "probe: the probe must read back into its own buffer\n"); ok = 0;
        } else {
            /* The memory type wiring: the probe buffer took the picker's choice. */
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(s_dev, s_probe.buf, &mr);
            uint32_t flags[VK_MAX_MEMORY_TYPES];
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                flags[i] = (uint32_t)mp.memoryTypes[i].propertyFlags;
            uint32_t want = cap_pick_mem_type(flags, mp.memoryTypeCount, mr.memoryTypeBits, NULL);
            if (want == UINT32_MAX || s_probe.mem_flags != flags[want]) {
                fprintf(stderr, "probe: the probe buffer must use the readback memory picker\n");
                ok = 0;
            }
        }
        CapTestImage pat;
        if (!cap_test_image_create(&pat, PSP_W, PSP_H) || !cap_test_image_upload(&pat)) {
            fprintf(stderr, "probe: pattern image setup failed\n"); ok = 0;
        } else {
            if (!sr_capture_arm("selftest_after_probe.ppm")) {
                fprintf(stderr, "probe: a file capture must be accepted while a probe is in "
                                "flight\n");
                ok = 0;
            }
            if (sdl3vk_present_image_ex((void *)pat.img, PSP_W, PSP_H) != 1 ||
                sr_capture_result() != 1 ||
                !cap_test_verify_ppm("selftest_after_probe.ppm", PSP_W, PSP_H)) {
                fprintf(stderr, "probe: the file capture after a probe must be exact\n"); ok = 0;
            }
            remove("selftest_after_probe.ppm");
            cap_test_image_destroy(&pat);
        }
        (void)sr_capture_probe_poll(1);
        if (sr_capture_probe_in_flight() || !sr_capture_probe_take(&b) || b != 8u) {
            fprintf(stderr, "probe: result %u (expected 8, the probe's own frame)\n", b); ok = 0;
        }

        /* The cost left once the GPU is done: invalidate and scan of a 2048x1088 dark
         * frame, the full-scan case. Reported, not asserted (host-dependent). */
        if (!sr_capture_arm_probe() ||
            sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != 1 ||
            !sdl3vk_wait_image((void *)dark.img)) {
            fprintf(stderr, "probe: timing probe setup failed\n"); ok = 0;
        }
        uint64_t t0 = SDL_GetTicksNS();
        (void)sr_capture_probe_poll(0);
        uint64_t t1 = SDL_GetTicksNS();
        if (sr_capture_probe_in_flight() || !sr_capture_probe_take(&b) || b != 8u) {
            fprintf(stderr, "probe: a completed readback must publish without waiting\n"); ok = 0;
        }
        fprintf(stderr, "probe: 2048x1088 dark frame scanned in %.3f ms (memory flags 0x%x)\n",
                (double)(t1 - t0) / 1e6, (unsigned)s_probe.mem_flags);

        /* Published before its frame slot is reused, with no blocking poll: after
         * PRESENT_FRAMES more presents the probe is no longer in flight. */
        if (!sr_capture_arm_probe() ||
            sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != 1) {
            fprintf(stderr, "probe: second probe setup failed\n"); ok = 0;
        }
        for (int i = 0; i < PRESENT_FRAMES; i++)
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "probe: present %d after the probe failed\n", i); ok = 0;
            }
        if (sr_capture_probe_in_flight() || !sr_capture_probe_take(&b) || b != 8u) {
            fprintf(stderr, "probe: not published before its frame slot was reused\n"); ok = 0;
        }

        /* A present that fails after the probe was recorded leaves no probe in flight and no
         * result, as before: the next present simply carries a new probe. */
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
        if (!sr_capture_arm_probe() ||
            sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != -1 ||
            sr_capture_probe_in_flight() || sr_capture_probe_take(&b)) {
            fprintf(stderr, "probe: a failed present must leave no probe and no result\n"); ok = 0;
        }
        (void)sdl3vk_wait_image((void *)dark.img);
        cap_test_image_destroy(&dark);
    }

    /* ---- production publication path -------------------------------------------------
     * FBSNAP's build/snapshots directory is not created by the build. The capture
     * publisher owns that boundary and must create it before opening its temp sibling. */
    {
        const char *p = "build/snapshots/selftest_frame.ppm";
        cap_test_rmdir("build/snapshots");
        cap_test_rmdir("build");
        if (!sr_capture_arm(p)) {
            fprintf(stderr, "pubdir: arm refused\n"); ok = 0;
        } else {
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "pubdir: present failed\n"); ok = 0;
            }
            if (sr_capture_result() != 1) {
                fprintf(stderr, "pubdir: result=%d (expected 1)\n",
                        sr_capture_result()); ok = 0;
            }
            if (!cap_test_verify_ppm(p, PSP_W, PSP_H)) {
                fprintf(stderr, "pubdir: PPM bytes wrong\n"); ok = 0;
            }
        }
        remove(p);
        cap_test_rmdir("build/snapshots");
        cap_test_rmdir("build");
    }

    /* ---- publication failure vs present truth, then recovery -------------------------
     * A regular file blocks the requested parent directory. The frame still reached the
     * presentation engine, so only the capture fails; removing the blocker must let the
     * immediately following request succeed. */
    {
        const char *p = "blocked/selftest_x.ppm";
        FILE *f = fopen("blocked", "wb");
        if (!f) {
            fprintf(stderr, "blocked: could not create file blocker\n"); ok = 0;
        } else {
            fputc(0, f); fclose(f);
        }
        if (!sr_capture_arm(p)) {
            fprintf(stderr, "blocked: arm refused\n"); ok = 0;
        } else {
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "blocked: present must succeed despite publication failure\n"); ok = 0;
            }
            if (sr_capture_result() != -1) {
                fprintf(stderr, "blocked: result=%d (expected -1)\n",
                        sr_capture_result()); ok = 0;
            }
        }
        {
            FILE *chk = fopen(p, "rb");
            if (chk) { fclose(chk); fprintf(stderr, "blocked: file must not exist\n"); ok = 0; }
        }
        remove(p);
        remove("blocked");

        if (!sr_capture_arm(p)) {
            fprintf(stderr, "blocked-recover: arm refused\n"); ok = 0;
        } else if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "blocked-recover: present failed\n"); ok = 0;
        } else if (sr_capture_result() != 1 || !cap_test_verify_ppm(p, PSP_W, PSP_H)) {
            fprintf(stderr, "blocked-recover: capture did not recover\n"); ok = 0;
        }
        remove(p);
        cap_test_rmdir("blocked");
    }

    /* ---- stale-arm and rejected-arm protection --------------------------------------
     * Cancellation represents a present slot dropped by the output cap. The next present
     * must not service that old path, and a refused replacement arm must not expose the
     * previous capture's result. */
    {
        const char *p = "selftest_stale.ppm";
        if (!sr_capture_arm(p)) {
            fprintf(stderr, "stale: arm refused\n"); ok = 0;
        }
        sr_capture_cancel();
        if (sr_capture_result() != 0) {
            fprintf(stderr, "stale: cancelled result=%d\n",
                    sr_capture_result()); ok = 0;
        }
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "stale: later present failed\n"); ok = 0;
        }
        {
            FILE *chk = fopen(p, "rb");
            if (chk) { fclose(chk); fprintf(stderr, "stale: cancelled arm published\n"); ok = 0; }
        }
        remove(p);

    }

    /* ---- repeated requests ---------------------------------------------------------- */
    {
        const char *p1 = "selftest_repeat_a.ppm";
        const char *p2 = "selftest_repeat_b.ppm";
        if (!sr_capture_arm(p1)) {
            fprintf(stderr, "repeat: first arm refused\n"); ok = 0;
        }
        if (sr_capture_arm(p2)) {
            fprintf(stderr, "repeat: second arm while pending accepted\n"); ok = 0;
        }
        if (sr_capture_result() != 0) {
            fprintf(stderr, "repeat: refused arm left stale result=%d\n", sr_capture_result()); ok = 0;
        }
        if (sdl3vk_present_rgba(px) != 1 || sr_capture_result() != 1) {
            fprintf(stderr, "repeat: first capture did not complete\n"); ok = 0;
        }
        if (!cap_test_verify_ppm(p1, PSP_W, PSP_H)) {
            fprintf(stderr, "repeat: first path PPM bytes wrong\n"); ok = 0;
        }
        {
            FILE *chk = fopen(p2, "rb");
            if (chk) { fclose(chk); fprintf(stderr, "repeat: refused path published\n"); ok = 0; }
        }
        remove(p1);
        remove(p2);
    }

    /* ---- present semaphores follow the swapchain image (#1799) ----------------------
     * A present's wait on its semaphore is known to be over only once its image is acquired
     * again; no fence covers it. Frames drawn from one slot put the frame ring out of step
     * with the swapchain images, as a driver that hands images out of order does, and none of
     * them may signal a semaphore that an earlier present of another image may still be
     * waiting on: the validation layer reports that as an ERROR. */
    {
        int errors0 = sdl3vk_validation_error_count();
        for (int i = 0; i < 8 * PRESENT_FRAMES; i++) {
            s_frame_cursor = 0;
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "present semaphores: present %d from one frame slot failed\n", i);
                ok = 0;
            }
        }
        if (sdl3vk_validation_error_count() != errors0) {
            fprintf(stderr, "present semaphores: %d validation error(s) presenting from one frame "
                            "slot (a semaphore signalled before its image was acquired again)\n",
                    sdl3vk_validation_error_count() - errors0);
            ok = 0;
        }
        /* Each image has one of its own, made with the swapchain; no slot past its images. */
        for (size_t i = 0; i < sizeof(s_swap_sem_done) / sizeof(s_swap_sem_done[0]); i++) {
            int dup = 0;
            for (size_t k = 0; k < i; k++) dup |= s_swap_sem_done[k] == s_swap_sem_done[i];
            if ((s_swap_sem_done[i] != VK_NULL_HANDLE) != (i < s_swap_n) ||
                (s_swap_sem_done[i] != VK_NULL_HANDLE && dup)) {
                fprintf(stderr, "present semaphores: slot %u of %u swapchain image(s) is wrong\n",
                        (unsigned)i, (unsigned)s_swap_n);
                ok = 0;
            }
        }
    }

    /* ---- present error recovery and disposition tests ------------------------------- */
    {
        /* Test 1: VK_SUBOPTIMAL_KHR (enqueued rebuild) */
        uint64_t sc0 = sdl3vk_swapchain_generation();
        sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present fault SUBOPTIMAL: expected present success (1)\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "present fault SUBOPTIMAL: expected swapchain rebuild\n"); ok = 0;
        }

        /* The enqueued present/rebuild path must still publish the capture recorded in
         * the same submission; rebuilding the swapchain cannot silently drop it. */
        sc0 = sdl3vk_swapchain_generation();
        if (!sr_capture_arm("selftest_subopt.ppm")) {
            fprintf(stderr, "present fault SUBOPTIMAL: capture arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present fault SUBOPTIMAL capture: present failed\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0 || sr_capture_result() != 1) {
            fprintf(stderr, "present fault SUBOPTIMAL capture: rebuild/result invalid\n"); ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_subopt.ppm", PSP_W, PSP_H)) {
            fprintf(stderr, "present fault SUBOPTIMAL capture: PPM bytes wrong\n"); ok = 0;
        }
        remove("selftest_subopt.ppm");

        /* Test 2: VK_ERROR_OUT_OF_DATE_KHR (enqueued present / stale swapchain) */
        sc0 = sdl3vk_swapchain_generation();
        uint64_t sem0 = sdl3vk_frame_semaphore_generation();
        if (!sr_capture_arm("selftest_ood.ppm")) {
            fprintf(stderr, "present fault OOD: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault OOD: expected present failure (-1)\n"); ok = 0;
        }
        if (sr_capture_result() != -1) {
            fprintf(stderr, "present fault OOD: capture result must fail (-1)\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "present fault OOD: expected swapchain rebuild\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() != sem0) {
            fprintf(stderr, "present fault OOD: enqueued present must NOT recreate semaphore\n"); ok = 0;
        }

        /* Test 3: VK_ERROR_OUT_OF_HOST_MEMORY (unenqueued hard error / semaphore recovery).
         * The image the failed present held stays acquired until the swapchain is rebuilt
         * (#1377), so the failure rebuilds it. */
        sem0 = sdl3vk_frame_semaphore_generation();
        sc0 = sdl3vk_swapchain_generation();
        if (!sr_capture_arm("selftest_oom.ppm")) {
            fprintf(stderr, "present fault OOM: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_HOST_MEMORY);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault OOM: expected present failure (-1)\n"); ok = 0;
        }
        if (sr_capture_result() != -1) {
            fprintf(stderr, "present fault OOM: capture result must fail (-1)\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() <= sem0) {
            fprintf(stderr, "present fault OOM: expected frame semaphore recovery/recreation\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0 || sdl3vk_renderer_terminal()) {
            fprintf(stderr, "present fault OOM: the still-acquired image must be handed back by a "
                            "swapchain rebuild\n"); ok = 0;
        }

        /* Verify frame slot recovery: next normal present must succeed cleanly without validation errors */
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present recovery: post-OOM normal present failed\n"); ok = 0;
        }
        /* More unenqueued presents in a row than the swapchain has images: without the rebuild
         * every one would keep an image and the next acquire would wait forever. */
        for (uint32_t k = 0; ok && k <= s_swap_n; k++) {
            sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_HOST_MEMORY);
            if (sdl3vk_present_rgba(px) != -1 || sdl3vk_renderer_terminal()) {
                fprintf(stderr, "present fault OOM: repeated failure %u was not recovered\n", (unsigned)k);
                ok = 0;
            }
        }
        if (ok && sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present recovery: present after repeated OOM failures failed\n"); ok = 0;
        }

    }

    /* A swapchain replacement with a frame still in flight waits until the old swapchain's
     * images are unused before destroying it; with validation on, destroying it earlier is
     * VUID-vkDestroySwapchainKHR-swapchain-01282. A failed wait keeps the swapchain (#1377). */
    {
        int in_flight = 0, after = 0;
        for (int i = 0; i < PRESENT_FRAMES; i++) in_flight += s_frame[i].submitted;
        uint64_t sc0 = sdl3vk_swapchain_generation();
        if (!create_swapchain() || sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "swapchain: replacement failed\n"); ok = 0;
        }
        for (int i = 0; i < PRESENT_FRAMES; i++) after += s_frame[i].submitted;
        if (in_flight == 0 || after != 0) {
            fprintf(stderr, "swapchain: replacement with %d frame(s) in flight left %d unwaited\n",
                    in_flight, after);
            ok = 0;
        }

        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "swapchain: present before the failed wait failed\n"); ok = 0;
        }
        VkSwapchainKHR kept = s_swap;
        sc0 = sdl3vk_swapchain_generation();
        s_idle_fault_result = VK_ERROR_DEVICE_LOST;
        if (create_swapchain() || s_swap != kept || sdl3vk_swapchain_generation() != sc0 ||
            !sdl3vk_renderer_terminal() || s_idle_fault_result != VK_SUCCESS) {
            fprintf(stderr, "swapchain: a failed queue-idle wait must keep the swapchain and "
                            "make the renderer terminal\n");
            ok = 0;
        }
    }

    /* Test 4: a quit consumed by a present is not lost. The present path drains the
     * event queue every frame, so a window close almost always lands there; the
     * process-exit decision is made later by sdl3vk_poll(). */
    {
        s_renderer_terminal = 0;
        create_swapchain();
        SDL_Event quit_ev;
        SDL_zero(quit_ev);
        quit_ev.type = SDL_EVENT_QUIT;
        if (!SDL_PushEvent(&quit_ev)) {
            fprintf(stderr, "quit: SDL_PushEvent failed: %s\n", SDL_GetError()); ok = 0;
        }
        (void)sdl3vk_present_rgba(px);
        if (sdl3vk_poll() != 0) {
            fprintf(stderr, "quit: a quit consumed by a present was lost; sdl3vk_poll() still reports alive\n");
            ok = 0;
        }
        s_quit_requested = 0;
    }

    int errors = sdl3vk_validation_error_count();
    if (errors != 0) {
        fprintf(stderr, "validation: %d ERROR-severity messages; expected 0\n", errors);
        ok = 0;
    }
    sdl3vk_shutdown();
    sdl3vk_shutdown();   /* after a full run (swapchain, overlay, capture, prescale): a no-op */
    errors = sdl3vk_validation_error_count();
    if (errors != 0) {
        fprintf(stderr, "validation: %d ERROR-severity teardown messages; expected 0\n", errors);
        ok = 0;
    }
    if (lifecycle_live_objects() != 0) {
        fprintf(stderr, "shutdown: %d object(s) alive after a full run\n", lifecycle_live_objects());
        ok = 0;
    }
    if (!ok) return 1;
    puts("gpu capture selftest: OK");
    return 0;
}

/* Runs the body from a scratch working directory beneath SR_SELFTEST_BUILD_ROOT. Its relative
 * paths (the FBSNAP default build/snapshots, the selftest_*.ppm frames, the input-profile
 * fixtures) resolve against the working directory, so they land in scratch, never in the
 * checkout's build/ or its root. The original working directory is restored on every path. */
int sdl3vk_capture_selftest(void) {
    static const char scratch_rel[] = SR_SELFTEST_BUILD_ROOT "/gpu_capture_selftest";
    char original[1024];
    char scratch[1024];
    if (!nk_platform_absolute_path(".", original, sizeof original) ||
        !nk_platform_mkdir_p(scratch_rel) ||
        !nk_platform_absolute_path(scratch_rel, scratch, sizeof scratch)) {
        fprintf(stderr, "gpu capture selftest: cannot prepare scratch directory '%s'\n",
                scratch_rel);
        return 1;
    }
    if (SDL3VK_TEST_CHDIR(scratch) != 0) {
        fprintf(stderr, "gpu capture selftest: cannot enter scratch directory '%s'\n", scratch);
        return 1;
    }
    int rc = sdl3vk_capture_selftest_body();
    if (SDL3VK_TEST_CHDIR(original) != 0) {
        fprintf(stderr, "gpu capture selftest: cannot restore working directory '%s'\n", original);
        return 1;
    }
    return rc;
}
