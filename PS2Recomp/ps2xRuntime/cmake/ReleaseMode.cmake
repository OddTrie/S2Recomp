include(CheckIPOSupported)

# LTO/LTCG makes linking the huge recompiled runtime take hours; keep it opt-in.
option(PS2X_ENABLE_LTO "Enable link-time optimization (/GL + /LTCG) for Release builds" OFF)

if(PS2X_ENABLE_LTO)
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
endif()

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()

        if(PS2X_ENABLE_LTO)
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/GL>) # whole program opt
            target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/LTCG>) # link-time code generation
        endif()
    endif()

    if(PS2X_ENABLE_LTO)
        if(IPO_SUPPORTED)
            set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        else()
            message(WARNING "Interprocedural optimization not supported: ${IPO_ERROR}")
        endif()
    else()
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
    endif()
endfunction()
