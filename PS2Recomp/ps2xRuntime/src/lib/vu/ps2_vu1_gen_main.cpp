// ps2xVuGen: the VU1 program generator alone (ps2_vu1_gen.cpp), so a build can write vu1gen/ from the
// player's own game files before it compiles the game.
//
//   ps2xVuGen <locations.toml> <extracted dir> <outDir>
//
// Includes the generator's source instead of listing it in the target: the runtime builds that file
// with clang-cl (a source property, which would apply to this target as well).
#include "ps2_vu1_gen.cpp"

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: ps2xVuGen <locations.toml> <extracted dir> <outDir>\n");
        return 2;
    }
    return ps2xVuGenerateDisc(argv[1], argv[2], argv[3]);
}
