add_rules("mode.release")

add_repositories("liteldev-repo https://github.com/LiteLDev/xmake-repo.git")
add_requires("detours v4.0.1-xmake.1")
add_requires("nlohmann_json v3.11.3")

target("ForceCloseOreUI")
    set_kind("shared")
    add_files("src/ForceCloseOreUI.cpp")
    set_languages("c++20")
    set_strip("all")
    add_packages("nlohmann_json", "detours")
    add_syslinks("version")       -- GetFileVersionInfo for writeLogHeader
    add_cxflags("/utf-8", "/EHa")
