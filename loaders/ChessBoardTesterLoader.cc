// ChessBoardTesterLoader.cc
//
// THE BOARD AS PIXELS, WITHOUT A WINDOW.
//
// A ChessBoard is a Drawable2D that owns its raster and redraws it when the
// game it is bound to changed (ChessBoard.h). This asks it the way the frame
// edge does -- Animating(), once per visit -- and reads the pixels back: the
// squares are the two colours, a piece is drawn where the FEN has one and not
// where it has none, a move moves the picture, the picked square and the last
// move are tinted, and a flipped board is the same picture the other way up.
// Then CChess's piece pictures (LoadSet, Skin): a piece keeps the picture of
// the half it started on; review shows a past ply and a press returns to live;
// a pawn reaching the last rank asks which piece, on the board.
//
//   ./Run_ChessBoardTesterLoader [out.ppm]
//
// No display, no GPU. With a path, the final raster is written as a PPM to
// look at.

#include "../ETCS.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string rid_arg(ETCS::Entity* e) { return std::to_string(e->getRID()); }

struct Px { int r, g, b, a; };

static Px at(Pixels_* p, int x, int y)
{
    const uint8_t* d = p->PixelData() + (static_cast<size_t>(y) * p->PixelWidth() + x) * 4;
    return Px{ d[0], d[1], d[2], d[3] };
}

static bool near(Px a, Px b, int tol = 8)
{
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

static void visit(ETCS::Entity* board)
{
    auto* an = static_cast<Animated_*>(board->getInterfacePointer(ETCS::Buffer("Animated")));
    if (an && an->Animating()) an->Advance();
}

// Any pixel in the square that is neither square colour: something is drawn.
static int inked(Pixels_* p, int file, int row, int sq, Px light, Px dark)
{
    int n = 0;
    for (int y = row * sq; y < (row + 1) * sq; ++y)
        for (int x = file * sq; x < (file + 1) * sq; ++x)
        {
            const Px c = at(p, x, y);
            if (!near(c, light) && !near(c, dark)) ++n;
        }
    return n;
}

int main(int argc, char** argv)
{
    WIRE_CONTEXT();

    ETCS::Entity* game  = ETCS::spawn_entity("ChessProvider", "ChessGame",  env, loader);
    ETCS::Entity* board = ETCS::spawn_entity("ChessProvider", "ChessBoard", env, loader);
    check(game && board, "a game and a board spawn");
    if (!game || !board) return 1;

    const int SIZE = 128, SQ = 16;
    board->call("ChessBoard.Create", "128", ctx);
    board->call("ChessBoard.Bind", rid_arg(game).c_str(), ctx);
    auto* px = static_cast<Pixels_*>(board->getInterfacePointer(ETCS::Buffer("Pixels")));
    check(px && px->PixelWidth() == SIZE && px->PixelHeight() == SIZE, "the raster is the size asked for");
    if (!px) return 1;

    visit(board);
    const Px light = at(px, 0, 0), dark = at(px, SQ, 0);
    check(!near(light, dark), "a8 and b8 are two colours");
    check(near(at(px, 0, SQ), dark) && near(at(px, SQ, SQ), light), "the colours alternate down the file too");
    check(near(at(px, SIZE - 1, SIZE - 1), light), "h1 is light (a light square on the right)");

    std::printf("\n-- pieces where the FEN has them ------------------------------\n");
    check(inked(px, 4, 7, SQ, light, dark) > 20, "e1 carries a piece (the king)");
    check(inked(px, 4, 6, SQ, light, dark) > 20, "e2 carries a piece (a pawn)");
    check(inked(px, 4, 4, SQ, light, dark) == 0, "e4 is empty");
    // A white piece is mostly light ink, a black piece mostly dark.
    {
        int bright = 0, total = 0;
        for (int y = 7 * SQ; y < 8 * SQ; ++y) for (int x = 4 * SQ; x < 5 * SQ; ++x)
        { const Px c = at(px, x, y); if (!near(c, light) && !near(c, dark)) { ++total; if (c.r > 200) ++bright; } }
        check(total > 0 && bright * 2 > total, "the white king is drawn in light ink");
        bright = 0; total = 0;
        for (int y = 0; y < SQ; ++y) for (int x = 4 * SQ; x < 5 * SQ; ++x)
        { const Px c = at(px, x, y); if (!near(c, light) && !near(c, dark)) { ++total; if (c.r < 80) ++bright; } }
        check(total > 0 && bright * 2 > total, "the black king is drawn in dark ink");
    }

    std::printf("\n-- a move moves the picture ------------------------------------\n");
    game->call("ChessGame.Act", "white move e2e4", ctx);
    visit(board);
    check(inked(px, 4, 4, SQ, light, dark) > 20, "after e2e4, e4 carries the pawn");
    // e2 is now empty but tinted as the move's origin: not the plain colour,
    // and no ink of the pawn's (its light face).
    {
        const Px c = at(px, 4 * SQ + 1, 6 * SQ + 1);
        check(!near(c, dark), "e2 is tinted as the last move's origin");
        int face = 0;
        for (int y = 6 * SQ; y < 7 * SQ; ++y) for (int x = 4 * SQ; x < 5 * SQ; ++x) if (at(px, x, y).r > 235) ++face;
        check(face == 0, "and carries no pawn any more");
    }

    std::printf("\n-- the picked square -------------------------------------------\n");
    const Px d2_before = at(px, 3 * SQ + 1, 6 * SQ + 1);
    board->call("ChessBoard.Select", "d2", ctx);
    visit(board);
    check(!near(at(px, 3 * SQ + 1, 6 * SQ + 1), d2_before), "d2 changes colour when picked");
    board->call("ChessBoard.Select", "", ctx);
    visit(board);
    check(near(at(px, 3 * SQ + 1, 6 * SQ + 1), d2_before), "and back when the pick is cleared");

    std::printf("\n-- flipped -----------------------------------------------------\n");
    board->call("ChessBoard.SetFlip", "1", ctx);
    visit(board);
    check(inked(px, 3, 3, SQ, light, dark) > 20, "flipped, the e4 pawn is drawn at the fourth row from the top, fourth file");
    check(inked(px, 3, 0, SQ, light, dark) > 20, "and the white king at the top");

    std::printf("\n-- the piece pictures --------------------------------------------\n");
    board->call("ChessBoard.SetFlip", "0", ctx);
    game->call("ChessGame.Act", "white reset", ctx);
    visit(board);
    auto sum = [&](int file, int row) { long t = 0; for (int y = row * SQ; y < (row + 1) * SQ; ++y) for (int x = file * SQ; x < (file + 1) * SQ; ++x) { const Px c = at(px, x, y); t += c.r * 3 + c.g * 5 + c.b * 7; } return t; };
    const long king_bitmap = sum(4, 7);
    std::string took;
    {
        ETCS::Buffer d("white ChessProvider/ChessProvider/CChess/piece_images/white");
        board->call("ChessBoard.LoadSet", d, ctx); took = d.toString();
    }
    check(took == "10", "the white set: all ten pictures");
    {
        ETCS::Buffer d("brown ChessProvider/ChessProvider/CChess/piece_images/brown");
        board->call("ChessBoard.LoadSet", d, ctx); took = d.toString();
    }
    check(took == "10", "the brown set, spelled in either case: all ten");
    {
        ETCS::Buffer d("none ChessProvider/no/such/dir");
        board->call("ChessBoard.LoadSet", d, ctx); took = d.toString();
    }
    check(took == "0", "a directory with no pictures is no set");
    std::string skin;
    { ETCS::Buffer d("white brown"); board->call("ChessBoard.Skin", d, ctx); skin = d.toString(); }
    check(skin == "ok", "white is drawn in the white set, black in the brown");
    { ETCS::Buffer d("white beige"); board->call("ChessBoard.Skin", d, ctx); skin = d.toString(); }
    check(skin == "no such set", "a set not loaded is refused");
    visit(board);
    check(inked(px, 4, 7, SQ, light, dark) > 20 && sum(4, 7) != king_bitmap, "e1 carries the king's picture, not the bitmap");
    auto picture = [&](const char* sq) { ETCS::Buffer d(sq); board->call("ChessBoard.PictureAt", d, ctx); return d.toString(); };
    check(picture("b1") == "N L" && picture("g1") == "N R" && picture("e1") == "K R" && picture("d8") == "q L", "a piece takes the half it starts on");
    game->call("ChessGame.Act", "white move b1c3", ctx);
    game->call("ChessGame.Act", "black move a7a6", ctx);
    game->call("ChessGame.Act", "white move c3e4", ctx);
    game->call("ChessGame.Act", "black move a6a5", ctx);
    game->call("ChessGame.Act", "white move g1f3", ctx);
    game->call("ChessGame.Act", "black move a5a4", ctx);
    game->call("ChessGame.Act", "white move e2e3", ctx);
    game->call("ChessGame.Act", "black move a4a3", ctx);
    game->call("ChessGame.Act", "white move f1e2", ctx);
    game->call("ChessGame.Act", "black move b7b6", ctx);
    game->call("ChessGame.Act", "white move e1g1", ctx);
    visit(board);
    check(picture("e4") == "N L" && picture("f3") == "N R", "a knight that crossed the middle keeps its picture");
    check(picture("g1") == "K R" && picture("f1") == "R R", "castled: the rook keeps the right-hand picture");

    std::printf("\n-- review ------------------------------------------------------\n");
    ETCS::Entity* table = ETCS::spawn_entity("ChessProvider", "ChessTable", env, loader);
    check(table != nullptr, "a table spawns");
    if (table)
    {
        table->call("ChessTable.Create", "", ctx);
        table->call("ChessTable.BindGame", rid_arg(game).c_str(), ctx);
        table->call("ChessTable.BindBoard", rid_arg(board).c_str(), ctx);
        auto view = [&]() { ETCS::Buffer d(""); table->call("ChessTable.View", d, ctx); return d.toString(); };
        check(view() == "11 11 0 live", "eleven plies, the last one shown");
        table->call("ChessTable.Do", "/back", ctx);
        table->call("ChessTable.Do", "/back", ctx);
        visit(board);
        check(view() == "9 11 0 review", "two back: ply 9 under review");
        check(picture("e1") == "K R" && picture("g1") == "-", "the board shows the king before it castled");
        table->call("ChessTable.Do", "/at 2w", ctx);
        check(view() == "3 11 0 review", "/at 2w: the board after white's second move");
        table->call("ChessTable.Key", "268", ctx);
        check(view() == "0 11 0 review", "Home: the start");
        table->call("ChessTable.Click", "72 72", ctx);
        check(view() == "11 11 0 live", "a press on the board while reviewing: live again");
        table->call("ChessTable.Key", "263", ctx);
        table->call("ChessTable.Keys", "0", ctx);
        table->call("ChessTable.Key", "263", ctx);
        check(view() == "10 11 0 review", "the left arrow steps back -- and not while the page's field has the keys");
        table->call("ChessTable.Keys", "1", ctx);
        table->call("ChessTable.Key", "256", ctx);
        check(view() == "11 11 0 live", "Esc: live");

        std::printf("\n-- promotion ---------------------------------------------------\n");
        table->call("ChessTable.Do", "/fen 8/4P1k1/8/8/8/8/6K1/8 w - - 0 1", ctx);
        table->call("ChessTable.Click", "72 24", ctx);
        table->call("ChessTable.Click", "72 8", ctx);
        visit(board);
        check(picture("e8") == "-" && picture("e7") == "P R", "a pawn to the last rank is not moved yet: the picker asks first");
        table->call("ChessTable.Click", "72 40", ctx);
        std::string fen;
        { ETCS::Buffer d(""); game->call("ChessGame.Fen", d, ctx); fen = d.toString(); }
        check(fen.compare(0, 13, "4R3/6k1/8/8/8") == 0, "the third piece down is a rook, and the pawn becomes one");
        table->call("ChessTable.Delete", "", ctx);
    }

    if (argc > 1)
    {
        if (FILE* f = std::fopen(argv[1], "wb"))
        {
            std::fprintf(f, "P6\n%d %d\n255\n", SIZE, SIZE);
            for (int y = 0; y < SIZE; ++y) for (int x = 0; x < SIZE; ++x)
            { const Px c = at(px, x, y); std::fputc(c.r, f); std::fputc(c.g, f); std::fputc(c.b, f); }
            std::fclose(f);
            std::printf("\nwrote %s\n", argv[1]);
        }
    }

    board->call("ChessBoard.Delete", "", ctx);
    game->call("ChessGame.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
