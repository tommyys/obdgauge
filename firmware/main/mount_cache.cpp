#include "mount_cache.h"

#include <cstdio>
#include <cstring>

#include "nvs.h"

namespace {

// One blob rather than six keys: the six numbers are only ever meaningful
// together, and a half-written set would be a plausible-looking wrong answer
// rather than an obviously absent one.
struct Blob {
    uint32_t version;
    double   fwd[3];
    double   down[3];
    double   weight;
};

constexpr uint32_t kVersion = 1;
constexpr const char* kNamespace = "mount";
constexpr const char* kKey = "axes";

}  // namespace

void mount_cache_load(gauge::GForce& g) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return;
    Blob b{};
    size_t len = sizeof b;
    const esp_err_t err = nvs_get_blob(h, kKey, &b, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof b || b.version != kVersion) return;
    gauge::MountAxes a{{b.fwd[0], b.fwd[1], b.fwd[2]},
                       {b.down[0], b.down[1], b.down[2]},
                       b.weight};
    g.restore_axes(a);          // refuses anything that is not a unit pair
    if (g.ready())
        printf("mount: restored, forward (%.3f %.3f %.3f)\n",
               a.fwd.x, a.fwd.y, a.fwd.z);
}

void mount_cache_save(const gauge::GForce& g) {
    const auto a = g.export_axes();
    if (!a) return;
    Blob b{kVersion,
           {a->fwd.x, a->fwd.y, a->fwd.z},
           {a->down.x, a->down.y, a->down.z},
           a->weight};
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, kKey, &b, sizeof b);
    nvs_commit(h);
    nvs_close(h);
}
