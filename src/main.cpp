/*
 * ROUTE BOT - mod de Geode (GD 2.2081, Geode 5.x)
 *
 * 1) ANALIZA el nivel (objetos, portales, orbes, picos, duracion estimada).
 * 2) CREA UNA RUTA con la fisica REAL del juego: busqueda en profundidad con checkpoints y retroceso
 *    (si una decision mata, vuelve atras y prueba otra). Se muestra progreso, tiempo y ETA en vivo.
 * 3) SIGUE LA RUTA: reinicia el nivel y reproduce los inputs paso a paso (240 TPS).
 *    Si se desincroniza y muere, re-analiza desde un checkpoint seguro y vuelve a seguirla.
 *
 * Solo funciona en MODO PRACTICA / PRUEBA (ajuste "practice-only"), para no tocar stats ni leaderboards.
 */
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <unordered_set>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

namespace rb {
    enum class Mode { Idle, Search, Follow };

    struct Node {                 // un punto de decision de la busqueda
        Ref<CheckpointObject> cp; // estado completo del juego en ese momento
        int  sc = 0;              // paso (step) en el que estamos
        bool prev = false;        // estado del boton al llegar
        bool chosen = false;      // opcion elegida desde este nodo
        int  tried = 0;           // bit0 = mantener probado, bit1 = cambiar probado
    };
    struct Flip { int sc; bool hold; };   // "en el paso sc, el boton pasa a hold"
    struct Stats { int hazards = 0, orbs = 0, pads = 0, modes = 0, speeds = 0; float maxX = 0, seconds = 0; };

    static int64_t cfg(char const* k) { return Mod::get()->getSettingValue<int64_t>(k); }
    static bool flag(char const* k) { return Mod::get()->getSettingValue<bool>(k); }
    static double msSince(Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    }
    static float speedOf(int id) {
        switch (id) { case 200: return 251.16f; case 202: return 387.42f; case 203: return 468.0f; case 1334: return 576.0f; default: return 311.58f; }
    }
    static std::string clockStr(double s) {
        if (s < 0) return "...";
        int m = (int)(s / 60), r = (int)s % 60;
        return fmt::format("{}:{:02}", m, r);
    }
}

class $modify(RoutePlayLayer, PlayLayer) {
    struct Fields {
        rb::Mode mode = rb::Mode::Idle;
        bool inSim = false, died = false, completed = false, needInit = false, lastHold = false;
        int sc = 0, replans = 0, safeAsc = 0, safeBsc = 0;
        bool safeAhold = false, safeBhold = false;
        size_t ri = 0;
        float acc = 0.f;
        std::deque<rb::Node> path;
        std::vector<rb::Flip> route;
        std::unordered_set<uint64_t> visited;
        Ref<CheckpointObject> safeA, safeB;
        Clock::time_point t0;
        double bestPct = 0;
        uint64_t steps = 0, expansions = 0, backtracks = 0;
        CCLabelBMFont* label = nullptr;
        rb::Stats stats;
    };

    // ------------------------------------------------------------------ utilidades
    float stepDt() {
        // un poco mas de 1/TPS para que cada llamada a update() avance exactamente 1 paso fisico
        return 1.f / (float)std::max<int64_t>(30, rb::cfg("tps")) * 1.0005f;
    }
    int blockSteps() { return (int)std::max<int64_t>(1, rb::cfg("block-steps")); }

    void setHold(bool h) {                       // pulsa / suelta el boton de forma "real"
        if (m_player1->m_holdingButton == h) return;
        this->handleButton(h, 1, true);
        if (m_gameState.m_isDualMode) this->handleButton(h, 1, false);
    }
    void forceHold(bool h) {                     // fija el estado del boton SIN generar evento (tras restaurar)
        m_player1->m_holdingButton = h;
        if (m_player2) m_player2->m_holdingButton = h;
    }
    void stepOnce() {                            // avanza UN paso fisico del juego
        auto& f = *m_fields;
        f.inSim = true;
        this->update(stepDt());
        f.inSim = false;
        f.sc++;
        f.steps++;
    }
    uint64_t stateKey(int block) {               // huella del estado para podar estados repetidos
        auto p = m_player1;
        uint64_t h = (uint64_t)block;
        auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix((uint64_t)(int64_t)std::lround(p->getPositionY() * 2.0));
        mix((uint64_t)(int64_t)std::lround(p->m_yVelocity * 4.0));
        mix((uint64_t)(int64_t)std::lround(p->getPositionX() * 0.25));
        uint64_t b = 0;
        b |= (uint64_t)p->m_isUpsideDown << 0;  b |= (uint64_t)p->m_isOnGround << 1;
        b |= (uint64_t)p->m_holdingButton << 2; b |= (uint64_t)p->m_isShip << 3;
        b |= (uint64_t)p->m_isBird << 4;        b |= (uint64_t)p->m_isBall << 5;
        b |= (uint64_t)p->m_isDart << 6;        b |= (uint64_t)p->m_isRobot << 7;
        b |= (uint64_t)p->m_isSpider << 8;      b |= (uint64_t)p->m_isSwing << 9;
        mix(b);
        return h;
    }

    // ------------------------------------------------------------------ 1) ANALISIS ESTATICO
    rb::Stats analyzeLevel() {
        rb::Stats s;
        std::vector<std::pair<float, int>> sp;
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            int id = obj->m_objectID;
            float x = obj->getPositionX();
            s.maxX = std::max(s.maxX, x);
            if (obj->m_objectType == GameObjectType::Hazard) s.hazards++;
            switch (id) {
                case 36: case 84: case 141: case 1022: case 1330: case 1333: case 1704: case 1751: s.orbs++; break;
                case 35: case 67: case 140: case 1332: s.pads++; break;
                case 12: case 13: case 47: case 111: case 660: case 745: case 1331: case 1933: s.modes++; break;
                case 200: case 201: case 202: case 203: case 1334: s.speeds++; sp.push_back({x, id}); break;
                default: break;
            }
        }
        std::sort(sp.begin(), sp.end());
        float x = 0, t = 0, v = 311.58f;
        for (auto& [px, id] : sp) { t += (px - x) / v; x = px; v = rb::speedOf(id); }
        t += (s.maxX - x) / v;
        s.seconds = t;
        return s;
    }

    // ------------------------------------------------------------------ 2) BUSQUEDA DE RUTA
    void startSearch(bool fresh) {
        auto& f = *m_fields;
        f.path.clear(); f.route.clear(); f.visited.clear();
        f.lastHold = false; f.died = f.completed = false;
        f.steps = f.expansions = f.backtracks = 0; f.bestPct = 0;
        if (fresh) f.replans = 0;
        f.safeA = nullptr; f.safeB = nullptr;
        this->resetLevelFromStart();
        f.sc = 0; f.t0 = Clock::now();
        f.needInit = true;                       // el nodo inicial se crea en el siguiente frame
        f.mode = rb::Mode::Search;
    }

    void runSearchSlice() {
        auto& f = *m_fields;
        if (f.needInit) {
            f.needInit = false;
            rb::Node n; n.cp = this->createCheckpoint(); n.sc = 0; n.prev = false;
            f.path.push_back(std::move(n));
            f.visited.insert(stateKey(0));
            return;
        }
        double budget = (double)std::max<int64_t>(2, rb::cfg("budget-ms"));
        auto t = Clock::now();
        while (f.mode == rb::Mode::Search) {
            if (rb::msSince(f.t0) > rb::cfg("max-seconds") * 1000.0) { finishSearch(false); break; }
            if (!expand()) break;
            if (rb::msSince(t) > budget) break;
        }
    }

    // Una expansion de la busqueda en profundidad. Devuelve false si la busqueda termino.
    bool expand() {
        auto& f = *m_fields;
        if (f.path.empty()) { finishSearch(false); return false; }
        rb::Node& n = f.path.back();

        if (n.tried == 3) {                      // ambas opciones murieron -> retroceder
            f.path.pop_back(); f.backtracks++;
            if (f.path.empty()) { finishSearch(false); return false; }
            return true;
        }
        bool opt;
        if (!(n.tried & 1)) { opt = n.prev;  n.tried |= 1; }   // primero: mantener el boton como estaba
        else                { opt = !n.prev; n.tried |= 2; }   // luego: cambiarlo
        n.chosen = opt;

        this->loadFromCheckpoint(n.cp);          // volver al estado del nodo
        f.sc = n.sc; f.died = f.completed = false;
        forceHold(n.prev);
        setHold(opt);

        int D = blockSteps();
        for (int i = 0; i < D; i++) {
            stepOnce();
            if (f.died || f.completed) break;
        }
        f.expansions++;
        if (f.died) return true;
        if (f.completed) { finishSearch(true); return false; }

        if (!f.visited.insert(stateKey(f.sc / D)).second) return true;   // estado ya visto -> podar

        rb::Node c; c.cp = this->createCheckpoint(); c.sc = f.sc; c.prev = opt; c.chosen = opt;
        f.path.push_back(std::move(c));
        f.bestPct = std::max(f.bestPct, (double)this->getCurrentPercent());

        size_t W = (size_t)std::max<int64_t>(30, rb::cfg("window"));
        while (f.path.size() > W) {              // fijar (commit) las decisiones antiguas y liberar memoria
            auto& fr = f.path.front();
            if (fr.chosen != f.lastHold) { f.route.push_back({fr.sc, fr.chosen}); f.lastHold = fr.chosen; }
            f.path.pop_front();
        }
        return true;
    }

    void finishSearch(bool ok) {
        auto& f = *m_fields;
        for (auto& n : f.path) {
            if (!n.tried) continue;
            if (n.chosen != f.lastHold) { f.route.push_back({n.sc, n.chosen}); f.lastHold = n.chosen; }
        }
        f.path.clear(); f.visited.clear();
        double secs = rb::msSince(f.t0) / 1000.0;
        if (f.steps > 2000) Mod::get()->setSavedValue<double>("ms-step", secs * 1000.0 / (double)f.steps);

        Notification::create(
            ok ? fmt::format("Route found in {:.1f}s ({} inputs)", secs, f.route.size())
               : fmt::format("Route incomplete: reached {:.1f}% in {:.1f}s", f.bestPct, secs),
            ok ? NotificationIcon::Success : NotificationIcon::Warning)->show();

        if (!f.route.empty() && rb::flag("auto-follow")) beginFollow();
        else { f.mode = rb::Mode::Idle; this->resetLevelFromStart(); }
    }

    // ------------------------------------------------------------------ 3) SEGUIR LA RUTA
    void beginFollow() {
        auto& f = *m_fields;
        f.mode = rb::Mode::Follow;
        f.safeA = nullptr; f.safeB = nullptr;
        f.died = f.completed = false;
        this->resetLevelFromStart();
        f.sc = 0; f.ri = 0; f.acc = 0.f;
    }

    void runFollow(float dt) {
        auto& f = *m_fields;
        float sd = 1.f / (float)std::max<int64_t>(30, rb::cfg("tps"));
        f.acc += dt;
        int n = 0;
        while (f.acc >= sd && n < 48 && f.mode == rb::Mode::Follow) {
            f.acc -= sd; n++;
            while (f.ri < f.route.size() && f.route[f.ri].sc <= f.sc) { setHold(f.route[f.ri].hold); f.ri++; }
            if (f.sc > 0 && f.sc % 120 == 0) {   // checkpoint "seguro" cada medio segundo (guardamos los 2 ultimos)
                f.safeA = f.safeB; f.safeAsc = f.safeBsc; f.safeAhold = f.safeBhold;
                f.safeB = this->createCheckpoint(); f.safeBsc = f.sc; f.safeBhold = m_player1->m_holdingButton;
            }
            stepOnce();
            if (f.died) { f.died = false; onFollowDeath(); return; }
        }
    }

    void onFollowDeath() {                       // desincronizado: re-analizar desde un punto seguro
        auto& f = *m_fields;
        if (++f.replans > (int)rb::cfg("max-replans")) {
            f.mode = rb::Mode::Idle;
            Notification::create("Route desynced - giving up", NotificationIcon::Error)->show();
            this->resetLevelFromStart();
            return;
        }
        if (!f.safeA) { startSearch(false); return; }
        this->loadFromCheckpoint(f.safeA);
        f.sc = f.safeAsc; forceHold(f.safeAhold);
        while (!f.route.empty() && f.route.back().sc > f.safeAsc) f.route.pop_back();
        f.lastHold = f.safeAhold;
        f.path.clear(); f.visited.clear();
        rb::Node n; n.cp = f.safeA; n.sc = f.safeAsc; n.prev = f.safeAhold;
        f.path.push_back(std::move(n));
        f.visited.insert(stateKey(f.sc / blockSteps()));
        f.died = f.completed = false; f.needInit = false;
        f.t0 = Clock::now(); f.steps = f.expansions = f.backtracks = 0; f.bestPct = 0;
        f.mode = rb::Mode::Search;
        Notification::create("Re-planning from a safe checkpoint...", NotificationIcon::Info)->show();
    }

    void stopBot() {
        auto& f = *m_fields;
        f.mode = rb::Mode::Idle; f.path.clear(); f.visited.clear(); f.inSim = false;
        this->resetLevelFromStart();
    }

    // ------------------------------------------------------------------ UI
    void updateHud() {
        auto& f = *m_fields;
        if (!f.label) return;
        if (f.mode == rb::Mode::Search) {
            double el = rb::msSince(f.t0) / 1000.0, pct = f.bestPct;
            double eta = pct > 1.0 ? el * (100.0 - pct) / pct : -1.0;
            f.label->setString(fmt::format("ANALYZING {:.1f}%  elapsed {}  ETA {}\nnodes {}  backtracks {}",
                pct, rb::clockStr(el), rb::clockStr(eta), f.expansions, f.backtracks).c_str());
        } else if (f.mode == rb::Mode::Follow) {
            f.label->setString(fmt::format("FOLLOWING ROUTE  {}/{} inputs  replans {}", f.ri, f.route.size(), f.replans).c_str());
        } else f.label->setString("");
    }

    void onBotButton(CCObject*) {
        auto& f = *m_fields;
        if (f.mode != rb::Mode::Idle) { stopBot(); return; }
        if (m_level->isPlatformer()) {
            FLAlertLayer::create("Route Bot", "Platformer levels are not supported.", "OK")->show();
            return;
        }
        if (rb::flag("practice-only") && !m_isPracticeMode && !m_isTestMode) {
            FLAlertLayer::create("Route Bot", "Enable <cy>Practice Mode</c> first.\n(Keeps stats and leaderboards clean.)", "OK")->show();
            return;
        }
        f.stats = analyzeLevel();
        double msStep = Mod::get()->getSavedValue<double>("ms-step", 0.4);
        double est = f.stats.seconds * (double)rb::cfg("tps") * msStep / 1000.0 * 1.5;
        auto txt = fmt::format(
            "<cy>Level analysis</c>\nLength: ~{:.1f}s\nHazards: {}\nOrbs: {}   Pads: {}\nMode portals: {}   Speed portals: {}\n\n"
            "Estimated search time: <cg>~{}</c> (live ETA while running)\nBlock: {} steps   Window: {} blocks",
            f.stats.seconds, f.stats.hazards, f.stats.orbs, f.stats.pads, f.stats.modes, f.stats.speeds,
            rb::clockStr(est), blockSteps(), rb::cfg("window"));
        createQuickPopup("Route Bot", txt, "Cancel", "Analyze + Follow", [this](FLAlertLayer*, bool ok) {
            if (ok) this->startSearch(true);
        });
    }

    // ------------------------------------------------------------------ hooks
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        auto spr = ButtonSprite::create("BOT");
        spr->setScale(.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(RoutePlayLayer::onBotButton));
        btn->setPosition({win.width - 34.f, win.height - 18.f});
        menu->addChild(btn);
        this->addChild(menu, 200);
        auto lbl = CCLabelBMFont::create("", "bigFont.fnt");
        lbl->setScale(.3f);
        lbl->setPosition({win.width / 2, win.height - 28.f});
        this->addChild(lbl, 201);
        m_fields->label = lbl;
    }

    void update(float dt) {
        auto& f = *m_fields;
        if (f.inSim || f.mode == rb::Mode::Idle) { PlayLayer::update(dt); return; }
        if (f.mode == rb::Mode::Search) runSearchSlice();
        else runFollow(dt);
        updateHud();
    }

    void destroyPlayer(PlayerObject* p, GameObject* o) {
        auto& f = *m_fields;
        if (f.mode != rb::Mode::Idle) { f.died = true; return; }   // sin animacion de muerte durante bot
        PlayLayer::destroyPlayer(p, o);
    }

    void levelComplete() {
        auto& f = *m_fields;
        if (f.mode == rb::Mode::Search) { f.completed = true; return; }  // no mostrar pantalla final mientras buscamos
        if (f.mode == rb::Mode::Follow) {
            f.mode = rb::Mode::Idle;
            Notification::create("Route completed!", NotificationIcon::Success)->show();
        }
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& f = *m_fields;
        if (f.mode == rb::Mode::Follow) { f.sc = 0; f.ri = 0; f.acc = 0.f; }
    }
};
