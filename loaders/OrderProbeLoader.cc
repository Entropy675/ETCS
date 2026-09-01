// Does a subtree destroy children-before-parent?
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "OrderProbe"
#endif
#include "../ETCS.h"
#include <iostream>
#include <string>
#include <vector>

static std::vector<std::string> g_order;

class Leaf : public ETCS::Entity
{
public:
    WIRE_TYPE_IDENTITY(Leaf)
    std::string name;
    ~Leaf() { g_order.push_back(name); }
};

int main()
{
    shell_startup();
    WIRE_CONTEXT();
    auto& arena = ETCS::MemoryArena::getInstance();

    Leaf* parent = arena.allocate<Leaf>();
    parent->name = "parent";

    Leaf* a = parent->addTag<Leaf>();  a->name = "child_a";
    Leaf* b = parent->addTag<Leaf>();  b->name = "child_b";
    Leaf* g = a->addTag<Leaf>();       g->name = "grandchild";

    std::cout << "--- deleting the subtree ---\n";
    arena.deleteEntity(parent, true);

    std::cout << "destruction order: ";
    for (const auto& n : g_order) std::cout << n << " ";
    std::cout << "\n";

    const bool children_first =
        g_order.size() == 4 && g_order.back() == "parent";
    std::cout << (children_first ? "FIFO: children before parent\n"
                                 : "NOT FIFO: parent ran before its children\n");
    return children_first ? 0 : 1;
}
