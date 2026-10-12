These synthetic ICC profiles are generated with LittleCMS by
`generate_profiles.py`. They contain standard sRGB, Display P3 (D65,
sRGB transfer curve), and Adobe RGB (D65, gamma 2.19921875) primaries.
`reference.json` records independent LittleCMS RGB8 transforms to sRGB
for input (191, 77, 51). Tests also compare encoded colors and metadata,
orientation, transparency, and original bytes. No external profile files
or licensing restrictions are involved.

The generated standard sRGB profile in `src/attachment/srgb_profile.hpp`
is used for validation transforms. JPEG XL profile preservation checks are deferred.
