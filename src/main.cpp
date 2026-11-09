#include "Runtime.hpp"
#include <pl/Mod.hpp>

class BackgroundVideoMod {
public:
    static BackgroundVideoMod& instance() {
        static BackgroundVideoMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) { return bgv::Runtime::get().load(context); }
    bool enable(pl::mod::ModContext& context) { return bgv::Runtime::get().enable(context); }
    bool disable(pl::mod::ModContext& context) { return bgv::Runtime::get().disable(context); }
    bool unload(pl::mod::ModContext& context) { return bgv::Runtime::get().unload(context); }
};

PL_REGISTER_MOD(BackgroundVideoMod, BackgroundVideoMod::instance())
