# cmake -DSRC=<mods source> -DDST=<mods next to the exe> -DAPI_HEADER=<s2r_mod_api.h> -P copy_mods.cmake
# Copies the bundled mods (readme, SDK, examples) next to the executable. A mod.ini that is already
# there is kept: it holds the user's settings (on/off, priority, save slot...). Everything else that
# ships is refreshed; files the user added are left alone.
# MultipleSaves was retired (save slots are built into the game now): remove its program files but
# keep the saves in its slots/ and deleted/ folders (the game copies slots/ into its own save slots).
file(REMOVE "${DST}/MultipleSaves/mod.dll" "${DST}/MultipleSaves/mod.ini")
file(REMOVE_RECURSE "${DST}/MultipleSaves/src")
file(GLOB_RECURSE files RELATIVE "${SRC}" "${SRC}/*")
foreach(rel IN LISTS files)
    get_filename_component(name "${rel}" NAME)
    if(name STREQUAL "mod.ini" AND EXISTS "${DST}/${rel}")
        continue()
    endif()
    get_filename_component(dir "${DST}/${rel}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    file(COPY_FILE "${SRC}/${rel}" "${DST}/${rel}" ONLY_IF_DIFFERENT)
endforeach()
# Empty folders of the example layout (files/DATA, textures).
file(GLOB_RECURSE dirs LIST_DIRECTORIES true RELATIVE "${SRC}" "${SRC}/*")
foreach(rel IN LISTS dirs)
    if(IS_DIRECTORY "${SRC}/${rel}")
        file(MAKE_DIRECTORY "${DST}/${rel}")
    endif()
endforeach()
if(API_HEADER)
    file(COPY_FILE "${API_HEADER}" "${DST}/_sdk/s2r_mod_api.h" ONLY_IF_DIFFERENT)
endif()
