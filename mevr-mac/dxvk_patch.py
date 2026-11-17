"""Applies the VR module hooks to RunnerRecompiled's DXVK 3.1.1 macOS tree (after pc/dxvk/build-macos.sh).
DXVK then creates its VkInstance / VkDevice through mevr_vk_create_instance / mevr_vk_create_device when the VR
module provides them (dlsym RTLD_DEFAULT), so the OpenXR runtime owns the Vulkan objects. Also adds the <cstddef>
include newer LLVM needs. Idempotent. usage: dxvk_patch.py <dxvk source tree>"""
import os, sys
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel); s = open(p).read(); changed = False
    for old, new, done in pairs:
        if done in s: continue
        if old not in s: sys.exit(f"{rel}: anchor not found: {old[:60]!r}")
        s = s.replace(old, new, 1); changed = True
    if changed: open(p, "w").write(s); print("patched", rel)

edit("src/util/util_math.h", [("#include <cmath>", "#include <cmath>\n#include <cstddef>\n#include <stddef.h>", "#include <cstddef>")])
edit("src/dxvk/dxvk_instance.cpp", [
    ("#include <version.h>", "#include <version.h>\n#include <dlfcn.h>", "#include <dlfcn.h>"),
    ("      VkResult status = m_vkl->vkCreateInstance(&info, nullptr, &instance);\n",
     """      // mevr: when the host process provides an OpenXR hook, the instance is created by the
      // OpenXR runtime (XR_KHR_vulkan_enable2) so the session can share it.
      typedef VkResult (*PFN_mevrCreateInstance)(PFN_vkGetInstanceProcAddr, const VkInstanceCreateInfo*, VkInstance*);
      auto mevrCreateInstance = reinterpret_cast<PFN_mevrCreateInstance>(dlsym(RTLD_DEFAULT, "mevr_vk_create_instance"));
      VkResult status = mevrCreateInstance
        ? mevrCreateInstance(m_vkl->getLoaderProc(), &info, &instance)
        : m_vkl->vkCreateInstance(&info, nullptr, &instance);
""", "mevr_vk_create_instance")])
edit("src/dxvk/dxvk_adapter.cpp", [
    ('#include "dxvk_adapter.h"', '#include <dlfcn.h>\n#include "dxvk_adapter.h"', "#include <dlfcn.h>"),
    ("    VkResult vr = vk->vkCreateDevice(m_handle, &deviceInfo, nullptr, &device);\n",
     """    // mevr: device creation through the OpenXR runtime (XR_KHR_vulkan_enable2), see dxvk_instance.cpp.
    typedef VkResult (*PFN_mevrCreateDevice)(PFN_vkGetInstanceProcAddr, VkPhysicalDevice, const VkDeviceCreateInfo*, VkDevice*);
    auto mevrCreateDevice = reinterpret_cast<PFN_mevrCreateDevice>(dlsym(RTLD_DEFAULT, "mevr_vk_create_device"));
    VkResult vr = mevrCreateDevice
      ? mevrCreateDevice(vk->getLoaderProc(), m_handle, &deviceInfo, &device)
      : vk->vkCreateDevice(m_handle, &deviceInfo, nullptr, &device);
""", "mevr_vk_create_device")])
