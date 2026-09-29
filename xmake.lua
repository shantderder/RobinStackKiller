-- SPDX-License-Identifier: GPL-3.0-or-later

includes("lib/commonlibsf")

set_project("RobinStackKiller")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

target("RobinStackKiller")
    add_rules("commonlibsf.plugin", {
        name = "RobinStackKiller",
        author = "OpenAI + user",
        description = "One-off recovery tool for unocRobinCheckSpecialistEffectScript.OnInit"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
