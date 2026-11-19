#ifndef BASE_PRESENT_H__
#define BASE_PRESENT_H__
#include "Present.h"
#include <mutex>
#include <set>

/*
 * THE ROLL LIVES HERE: who is here, and the hall this entity is listed in
 * as its session's owner, with what it last said there.
 *
 * ITS OWN LOCK, taken before the leaf's: the hooks below are called with
 * it held and may take the leaf's own. Nothing here is called under the
 * leaf's lock, and no hall is called under either -- a hall is a surface
 * of the site's directory, so an advert is a round trip.
 *
 *   KindConcrete      the word the hall lists it under ("kart", "golf")
 *   HallConcrete      the owner's hall line after "<kind> <id>" (default: none)
 *   ArrivedConcrete / DepartedConcrete   who came, who went (default: nothing)
 */
ETCS_SUPERTYPE_BASE(Present)
{
    ETCS_MAKE_INSTANCE(Present)

    virtual std::string KindConcrete() const = 0;
    virtual std::string HallConcrete() { return std::string(); }
    virtual void        ArrivedConcrete(const std::string& who) { (void)who; }
    virtual void        DepartedConcrete(const std::string& who) { (void)who; }

    // The owner of a session listed in `hall` as `name` under `id`.
    void Attend(ETCS::RID hall, const std::string& name, const std::string& id)
    {
        Absent();
        std::lock_guard<std::mutex> g(m_present_mu);
        m_p_hall = hall; m_p_name = name; m_p_id = id;
    }
    // Out of any session: the hall's line withdrawn, the roll emptied (and
    // every stream still open of the last session departs into nothing).
    void Absent()
    {
        ETCS::RID hall = 0; std::string name;
        {
            std::lock_guard<std::mutex> g(m_present_mu);
            ++m_pgen;
            hall = m_p_hall; name = m_p_name;
            m_p_hall = 0; m_p_name.clear(); m_p_id.clear(); m_p_said.clear();
            m_p_here.clear();
        }
        if (hall && !name.empty()) tellHall(hall, "Withdraw", name);
    }

    uint64_t Arrive(const std::string& who) override
    {
        std::lock_guard<std::mutex> g(m_present_mu);
        if (who.empty() || !m_p_here.insert(who).second) return 0;
        ArrivedConcrete(who);
        return m_pgen;
    }
    void Depart(uint64_t gen, const std::string& who) override
    {
        std::lock_guard<std::mutex> g(m_present_mu);
        if (gen != m_pgen || !m_p_here.erase(who)) return;
        DepartedConcrete(who);
    }
    std::string Here() override
    {
        std::lock_guard<std::mutex> g(m_present_mu);
        std::string out;
        for (const auto& w : m_p_here) out += w + "\n";
        return out;
    }
    void Announce() override
    {
        std::string line;
        ETCS::RID hall = 0;
        {
            std::lock_guard<std::mutex> g(m_present_mu);
            if (!m_p_hall || m_p_name.empty()) return;
            const std::string extra = HallConcrete();
            line = m_p_name + " " + KindConcrete() + " " + (m_p_id.empty() ? "-" : m_p_id) + (extra.empty() ? "" : " " + extra);
            if (line == m_p_said) return;
            m_p_said = line; hall = m_p_hall;
        }
        tellHall(hall, "Advertise", line);
    }

private:
    // A directory's verb by its own tag (a Lobby, or a surface of one).
    static void tellHall(ETCS::RID rid, const char* verb, const std::string& arg)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        if (!e) return;
        ETCS::Buffer a; a.writeString(arg.c_str());
        try { e->call(ETCS::Buffer((e->getSourceTag().toString() + "." + verb).c_str()), a); } catch (...) {}
    }

    std::mutex            m_present_mu;
    uint64_t              m_pgen = 1;     // 0 is Arrive's "not an arrival"
    ETCS::RID             m_p_hall = 0;
    std::string           m_p_name, m_p_id, m_p_said;
    std::set<std::string> m_p_here;
};

#endif // BASE_PRESENT_H__
