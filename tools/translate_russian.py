#!/usr/bin/env python3
"""Translate all address-dependent tables and files from English to Russian edition.

Usage:
  python3 tools/translate_russian.py [--map rus_address_map.json]
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_PATH = os.path.join(ROOT, "rus_address_map.json")

def load_map(path=MAP_PATH):
    if not os.path.exists(path):
        sys.exit(f"Error: map {path} not found. Run tools/rus_address_map.py first.")
    with open(path) as fp:
        raw = json.load(fp)
    # create normalized mapping dict
    mapping = {}
    for k, v in raw.items():
        k_hex = k.replace("sub_", "").replace("SUB_", "").upper()
        v_hex = v.replace("sub_", "").replace("SUB_", "").upper()
        mapping[f"sub_{k_hex}"] = f"sub_{v_hex}"
        mapping[f"sub_{k_hex.lower()}"] = f"sub_{v_hex.lower()}"
        mapping[f"SUB_{k_hex}"] = f"SUB_{v_hex}"
        mapping[f"0x{k_hex}"] = f"0x{v_hex}"
        mapping[f"0x{k_hex.lower()}"] = f"0x{v_hex.lower()}"
        mapping[k_hex] = v_hex
        mapping[k_hex.lower()] = v_hex.lower()
    return mapping, raw


def translate_pch_scripts(m):
    # 1. tools/pch_ui_viewport.py
    vp_path = os.path.join(ROOT, "tools", "pch_ui_viewport.py")
    vp_txt = open(vp_path).read()
    # sub_82238FE8 -> sub_82238DD8
    old_target = "sub_82238FE8"
    new_target = m.get(old_target, "sub_82238DD8")
    vp_txt_new = vp_txt.replace(old_target, new_target)
    if vp_txt_new != vp_txt:
        open(vp_path, "w").write(vp_txt_new)
        print(f"Updated {vp_path}: {old_target} -> {new_target}")

    # 2. tools/pch_ui_world_to_screen.py
    w2s_path = os.path.join(ROOT, "tools", "pch_ui_world_to_screen.py")
    w2s_txt = open(w2s_path).read()
    old_target_w = "sub_827C07F0"
    new_target_w = m.get(old_target_w, "sub_827C10E8")
    w2s_txt_new = w2s_txt.replace(old_target_w, new_target_w)
    if w2s_txt_new != w2s_txt:
        open(w2s_path, "w").write(w2s_txt_new)
        print(f"Updated {w2s_path}: {old_target_w} -> {new_target_w}")


def translate_codegen_sh(m):
    cdg_path = os.path.join(ROOT, "tools", "codegen.sh")
    txt = open(cdg_path).read()

    # --noreturn=82ACA550 -> --noreturn=82BE2330
    old_nr = "82ACA550"
    new_nr = m.get(old_nr, "82BE2330")
    txt = txt.replace(f"--noreturn={old_nr}", f"--noreturn={new_nr}")

    # check_residue:
    # sub_82ACA670 -> sub_82BE2450
    # sub_826710B0 -> sub_82671C38
    # sub_827F6558 -> sub_827F6D88
    # sub_82BE7694 -> sub_82BE6984
    for old_s in ["sub_82ACA670", "sub_826710B0", "sub_827F6558", "sub_82BE7694"]:
        new_s = m.get(old_s, old_s)
        txt = txt.replace(old_s, new_s)

    open(cdg_path, "w").write(txt)
    print(f"Updated {cdg_path}")


def translate_perf_overrides(m):
    perf_path = os.path.join(ROOT, "app", "perf_overrides.toml")
    txt = open(perf_path).read()

    # lr_keep_returns = [0x826E5ABC, 0x826E5AEC, 0x8223D0D4, 0x826E7CE8]
    # Russian returns: [0x826E62B4, 0x826E62E4, 0x8223CE7C, 0x826E84E0]
    txt = re.sub(
        r"lr_keep_returns\s*=\s*\[.*?\]",
        "lr_keep_returns = [0x826E62B4, 0x826E62E4, 0x8223CE7C, 0x826E84E0]",
        txt
    )

    # Functions table:
    # 0x82AC65E4 -> 0x82974C54
    # and 0x8260D924
    new_funcs = '''[functions]
"0x82974C54" = { share_registers = true }
"0x8260D804" = { share_registers = true }
"0x8260D828" = { share_registers = true }
"0x8260D864" = { share_registers = true }
"0x8260D8C4" = { share_registers = true }
"0x8260D924" = { share_registers = true }
"0x8260D948" = { share_registers = true }
"0x82736208" = { share_registers = true }
"0x82B2A564" = { share_registers = true }
"0x82B2A5A4" = { share_registers = true }
'''
    if "[functions]" in txt:
        txt = re.sub(r"\[functions\].*", new_funcs, txt, flags=re.DOTALL)
    else:
        txt += "\n" + new_funcs

    open(perf_path, "w").write(txt)
    print(f"Updated {perf_path}")


def translate_masseffect_app_h():
    h_path = os.path.join(ROOT, "app", "src", "masseffect_app.h")
    txt = open(h_path).read()
    # 0x82F7811C -> 0x82F80B1C
    txt = txt.replace("0x82F7811C", "0x82F80B1C")
    open(h_path, "w").write(txt)
    print(f"Updated {h_path}: kHashAddress -> 0x82F80B1C")


def translate_d3d_and_shaders(m):
    d3d_path = os.path.join(ROOT, "app", "src", "me_d3d_trace.cpp")
    txt = open(d3d_path).read()
    
    # Replacement map for D3D entries
    d3d_map = {
        "82227760": "82227550",
        "82227C40": "82227A30",
        "82228178": "82227F68",
        "82228568": "82228358",
        "8221E298": "8221E088",
        "82234D98": "82234B88",
        "82224698": "82224488",
        "82224528": "82224318",
        "82234000": "82233DF0",
    }
    for old_a, new_a in d3d_map.items():
        txt = txt.replace(old_a, new_a)
        txt = txt.replace(f"0x{old_a}", f"0x{new_a}")
        txt = txt.replace(f"0x{old_a.lower()}", f"0x{new_a.lower()}")
    open(d3d_path, "w").write(txt)
    print(f"Updated {d3d_path}")

    # Shader dump
    sd_path = os.path.join(ROOT, "app", "src", "me_shader_dump.cpp")
    sd_txt = open(sd_path).read()
    sd_txt = sd_txt.replace("sub_8222F850", "sub_8222F640")
    sd_txt = sd_txt.replace("sub_8222F1C8", "sub_8222EFB8")
    open(sd_path, "w").write(sd_txt)
    print(f"Updated {sd_path}")


def translate_me_native(m):
    # 1. me_resolution.cpp
    res_path = os.path.join(ROOT, "app", "src", "native", "me_resolution.cpp")
    txt = open(res_path).read()
    txt = txt.replace("sub_826E5060", "sub_826E5858")
    txt = txt.replace("sub_823D0118", "sub_823D0CE8")
    txt = txt.replace("sub_823D1520", "sub_823D20F0")
    txt = txt.replace("sub_823D1460", "sub_823D2030")
    txt = txt.replace("0x823D1520", "0x823D20F0")
    txt = txt.replace("0x823D1460", "0x823D2030")
    txt = txt.replace("0x82EC285C", "0x82EC287C")
    txt = txt.replace("0x82234D98", "0x82234B88")
    txt = txt.replace("0x82224698", "0x82224488")
    txt = txt.replace("0x82224528", "0x82224318")
    txt = txt.replace("0x826E5ABC", "0x826E62B4")
    txt = txt.replace("0x826E5AEC", "0x826E62E4")
    txt = txt.replace("sub_823CF0D8", "sub_823CFCA8")
    txt = txt.replace("0x8223D0D4", "0x8223CE7C")
    open(res_path, "w").write(txt)
    print(f"Updated {res_path}")

    # 2. me_ring_wait.cpp
    rw_path = os.path.join(ROOT, "app", "src", "native", "me_ring_wait.cpp")
    rw_txt = open(rw_path).read()
    rw_txt = rw_txt.replace("sub_822FE760", "sub_822FE560")
    rw_txt = rw_txt.replace("sub_82811750", "sub_82811F80")
    rw_txt = rw_txt.replace("0x826E7CE8", "0x826E84E0")
    open(rw_path, "w").write(rw_txt)
    print(f"Updated {rw_path}")

    # 3. me_physx.cpp
    px_path = os.path.join(ROOT, "app", "src", "native", "me_physx.cpp")
    px_txt = open(px_path).read()
    px_txt = px_txt.replace("sub_82AF1E88", "sub_8299D160")
    open(px_path, "w").write(px_txt)
    print(f"Updated {px_path}")

    # 4. me_audio_hooks.cpp and me_audio_dsp.h
    ah_path = os.path.join(ROOT, "app", "src", "native", "me_audio_hooks.cpp")
    ah_txt = open(ah_path).read()
    ah_txt = ah_txt.replace("sub_82AA53C0", "sub_82B1AC90")
    ah_txt = ah_txt.replace("sub_82B4D580", "sub_82B46868")
    ah_txt = ah_txt.replace("sub_82AA65E8", "sub_82B1BE90")
    ah_txt = ah_txt.replace("Native82AA53C0", "Native82B1AC90")
    ah_txt = ah_txt.replace("Native82B4D580", "Native82B46868")
    open(ah_path, "w").write(ah_txt)
    print(f"Updated {ah_path}")

    ad_path = os.path.join(ROOT, "app", "src", "native", "me_audio_dsp.h")
    ad_txt = open(ad_path).read()
    ad_txt = ad_txt.replace("sub_82AA53C0", "sub_82B1AC90")
    ad_txt = ad_txt.replace("sub_82B4D580", "sub_82B46868")
    ad_txt = ad_txt.replace("sub_82AA65E8", "sub_82B1BE90")
    ad_txt = ad_txt.replace("Native82AA53C0", "Native82B1AC90")
    ad_txt = ad_txt.replace("Native82B4D580", "Native82B46868")
    ad_txt = ad_txt.replace("0x82B4D5A0", "0x82B46888")
    ad_txt = ad_txt.replace("0x821BE0F0u", "0x821BE0C0u")
    open(ad_path, "w").write(ad_txt)
    print(f"Updated {ad_path}")


def translate_hot_guest():
    # As instructed by TASK-RUS-EDITION.md: disable native replacements that cannot be mapped or verified.
    # Disabling ME_HOT_HOOK ensures no link-time undefined references or runtime mismatches with Russian XEX.
    hg_path = os.path.join(ROOT, "app", "src", "native", "me_hot_guest.cpp")
    txt = open(hg_path).read()
    # Wrap ME_HOT_HOOK definitions in #if 0 ... #endif
    if "#if 0 // Disabled for Russian edition" not in txt:
        txt = re.sub(
            r"(ME_HOT_HOOK\([^)]+\))",
            r"// \1",
            txt
        )
        txt = "// Native replacements disabled for Russian edition (recompiled XEX originals used)\n" + txt
        open(hg_path, "w").write(txt)
        print(f"Updated {hg_path}: disabled ME_HOT_HOOK calls")


def translate_function_order(m):
    ld_path = os.path.join(ROOT, "app", "function_order.ld")
    if not os.path.exists(ld_path):
        return
    txt = open(ld_path).read()
    
    def repl(match):
        sym = match.group(1).lower()
        if sym in m:
            return match.group(0).replace(match.group(1), m[sym])
        return match.group(0)

    # Match patterns like *(.text.__imp__sub_82xxxxxx) or *(.text.sub_82xxxxxx)
    new_txt = re.sub(r"\b(sub_[0-9A-Fa-f]{8})\b", repl, txt)
    open(ld_path, "w").write(new_txt)
    print(f"Updated {ld_path} with mapped function symbols")


def main():
    m_ext, raw = load_map()
    translate_pch_scripts(m_ext)
    translate_codegen_sh(m_ext)
    translate_perf_overrides(m_ext)
    translate_masseffect_app_h()
    translate_d3d_and_shaders(m_ext)
    translate_me_native(m_ext)
    translate_hot_guest()
    translate_function_order(m_ext)
    print("\nAll translations successfully applied!")


if __name__ == "__main__":
    main()
