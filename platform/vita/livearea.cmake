# Optional VPK images from platform/vita/logo_livearea (README.md there): each file
# present is packaged, a missing one keeps the Vita's default. Sets VITA_LIVEAREA_FILES
# (FILE arguments for vita_create_vpk) and VITA_LIVEAREA_SUMMARY.
set(VITA_LIVEAREA_DIR "${CMAKE_CURRENT_LIST_DIR}/logo_livearea")
set(VITA_LIVEAREA_FILES)
set(VITA_LIVEAREA_FOUND)

# The Vita installs only 8-bit indexed PNGs of the exact size (else a failed install
# or a blank image), so a wrong file stops the configure instead.
function(vita_livearea_check_png path width height)
    file(READ "${path}" header LIMIT 26 HEX)
    string(SUBSTRING "${header}" 0 16 signature)
    if(NOT signature STREQUAL "89504e470d0a1a0a")
        message(FATAL_ERROR "${path}: not a PNG file")
    endif()
    string(SUBSTRING "${header}" 32 8 w)
    string(SUBSTRING "${header}" 40 8 h)
    string(SUBSTRING "${header}" 48 2 depth)
    string(SUBSTRING "${header}" 50 2 type)
    math(EXPR w "0x${w}")
    math(EXPR h "0x${h}")
    if(NOT w EQUAL width OR NOT h EQUAL height)
        message(FATAL_ERROR "${path}: ${w}x${h}, the Vita needs ${width}x${height}")
    endif()
    if(NOT depth STREQUAL "08" OR NOT type STREQUAL "03")
        message(FATAL_ERROR "${path}: not an 8-bit indexed PNG (256-colour palette); "
            "convert it, e.g. pngquant --force --output out.png 256 in.png")
    endif()
endfunction()

foreach(entry "icon0.png;128;128;sce_sys/icon0.png"
              "bg.png;840;500;sce_sys/livearea/contents/bg.png"
              "startup.png;280;158;sce_sys/livearea/contents/startup.png")
    list(GET entry 0 name)
    list(GET entry 1 width)
    list(GET entry 2 height)
    list(GET entry 3 destination)
    set(path "${VITA_LIVEAREA_DIR}/${name}")
    if(EXISTS "${path}")
        vita_livearea_check_png("${path}" ${width} ${height})
        list(APPEND VITA_LIVEAREA_FILES FILE "${path}" "${destination}")
        list(APPEND VITA_LIVEAREA_FOUND "${name}")
    endif()
endforeach()
# Re-run the configure when an image is added or removed.
if(IS_DIRECTORY "${VITA_LIVEAREA_DIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VITA_LIVEAREA_DIR}")
endif()

# bg.png and startup.png are shown only through template.xml: the folder's own, or
# a minimal one naming the images present.
if("bg.png" IN_LIST VITA_LIVEAREA_FOUND OR "startup.png" IN_LIST VITA_LIVEAREA_FOUND OR
   EXISTS "${VITA_LIVEAREA_DIR}/template.xml")
    set(template "${VITA_LIVEAREA_DIR}/template.xml")
    if(EXISTS "${template}")
        list(APPEND VITA_LIVEAREA_FOUND "template.xml")
    else()
        set(template "${CMAKE_CURRENT_BINARY_DIR}/livearea_template.xml")
        set(xml "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<livearea style=\"a1\" format-ver=\"01.00\" content-rev=\"1\">\n")
        if("bg.png" IN_LIST VITA_LIVEAREA_FOUND)
            string(APPEND xml "  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n")
        endif()
        if("startup.png" IN_LIST VITA_LIVEAREA_FOUND)
            string(APPEND xml "  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n")
        endif()
        string(APPEND xml "</livearea>\n")
        file(CONFIGURE OUTPUT "${template}" CONTENT "${xml}" @ONLY) # rewritten only when changed
        list(APPEND VITA_LIVEAREA_FOUND "template.xml (generated)")
    endif()
    list(APPEND VITA_LIVEAREA_FILES FILE "${template}" "sce_sys/livearea/contents/template.xml")
endif()

if(VITA_LIVEAREA_FOUND)
    string(REPLACE ";" ", " VITA_LIVEAREA_SUMMARY "${VITA_LIVEAREA_FOUND}")
else()
    set(VITA_LIVEAREA_SUMMARY "none (Vita defaults)")
endif()
