/*
 * ROUTE BOT v2 - mod de Geode (Geode 5.x, GD 2.2081). Solo se cambia src/main.cpp (mod.json igual).
 *
 *  BOT  = busqueda rapida (se ve el nivel avanzando a toda velocidad).
 *  MAX  = "ANALIZAR AL MAXIMO": pantalla de carga + analisis de todos los hitbox peligrosos del nivel,
 *         bloques de decision segun el VEHICULO (cube/ship/wave/...), elige siempre el camino con MAS
 *         margen de seguridad respecto a los picos, y si se atasca reintenta desde puntos anteriores
 *         hasta resolverlo (o agotar el tiempo maximo).
 *  TRAIL = cambia el color de la trayectoria (verde, cian, amarillo, magenta, blanco, naranja, rojo, oculta).
 *
 * Solo en MODO PRACTICA / PRUEBA (ajuste "practice-only").
 */
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

namespace rb {
    enum class Mode { Idle, Search, Follow };
    enum class Veh { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

    struct Node {                       // punto de decision de la busqueda
        Ref<CheckpointObject> cp;       // estado completo del juego
        int sc = 0;                     // paso fisico
        bool prev = false;              // boton al llegar
        bool chosen = false;            // opcion elegida desde aqui
        bool init = false;              // ya preparado (orden de opciones, vehiculo, bloque)
        bool ord[2] = {false, false};   // orden de prueba de las opciones
        int cnt = 2, next = 0;          // opciones validas / siguiente a probar
        int D = 4;                      // pasos de este bloque
        float x = 0, y = 0;             // posicion del jugador
        uint64_t key = 0;               // huella del estado
    };
    struct Flip { int sc; bool hold; };
    struct TrailPt { int sc; float x, y; bool hold; };
    struct Anchor {                     // punto de reinicio seguro (para reintentar si se atasca)
        Ref<CheckpointObject> cp; int sc = 0; bool prev = false, lastHold = false;
        size_t routeN = 0, trailN = 0, keyN = 0; float x = 0, y = 0; uint64_t key = 0; bool valid = false;
    };
    struct Res { bool died = false, completed = false; uint64_t key = 0; float clr = 0, x = 0, y = 0; };
    struct Stats { int hazards = 0, orbs = 0, pads = 0, modes = 0, speeds = 0; float maxX = 0, seconds = 0; };

    static int64_t cfg(char const* k) { return Mod::get()->getSettingValue<int64_t>(k); }
    static bool flag(char const* k) { return Mod::get()->getSettingValue<bool>(k); }
    static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
    static float speedOf(int id) {
        switch (id) { case 200: return 251.16f; case 202: return 387.42f; case 203: return 468.0f; case 1334: return 576.0f; default: return 311.58f; }
    }
    static std::string clockStr(double s) {
        if (s < 0) return "...";
        return fmt::format("{}:{:02}", (int)(s / 60), (int)s % 60);
    }
    static char const* vehName(Veh v) {
        switch (v) { case Veh::Ship: return "Ship"; case Veh::Ball: return "Ball"; case Veh::Ufo: return "UFO"; case Veh::Wave: return "Wave";
                     case Veh::Robot: return "Robot"; case Veh::Spider: return "Spider"; case Veh::Swing: return "Swing"; default: return "Cube"; }
    }
    static bool isTap(Veh v) { return v == Veh::Cube || v == Veh::Robot || v == Veh::Spider || v == Veh::Ball || v == Veh::Ufo; }
    static char const* TRAIL_NAMES[8] = {"Green", "Cyan", "Yellow", "Magenta", "White", "Orange", "Red", "Hidden"};
    static ccColor4F TRAIL_COLORS[7] = {
        {0.10f, 1.00f, 0.30f, 0.9f}, {0.20f, 0.90f, 1.00f, 0.9f}, {1.00f, 0.90f, 0.20f, 0.9f}, {1.00f, 0.30f, 0.90f, 0.9f},
        {1.00f, 1.00f, 1.00f, 0.9f}, {1.00f, 0.55f, 0.10f, 0.9f}, {1.00f, 0.20f, 0.20f, 0.9f}};
}

class $modify(RoutePlayLayer, PlayLayer) {
    struct Fields {
        rb::Mode mode = rb::Mode::Idle;
        bool inSim = false, died = false, completed = false, needInit = false, lastHold = false, hold = false, maxMode = false;
        int sc = 0, replans = 0, restarts = 0, safeAsc = 0, safeBsc = 0, deaths = 0, dStep = 0, dObj = -1, bestSc = 0;
        bool safeAhold = false, safeBhold = false;
        size_t ri = 0, commitCount = 0;
        float acc = 0.f;
        std::deque<rb::Node> path;
        std::vector<rb::Flip> route;
        std::vector<rb::TrailPt> trail;
        std::vector<std::pair<int, uint64_t>> commitKeys;
        std::deque<rb::Anchor> anchors;
        rb::Anchor startAnchor;
        std::unordered_set<uint64_t> visited;
        std::unordered_map<int, std::vector<CCRect>> haz;   // hitbox peligrosos por cubeta de 30 unidades en X
        Ref<CheckpointObject> safeA, safeB;
        Ref<CCDrawNode> trailNode;
        Clock::time_point t0;
        double bestPct = 0;
        uint64_t steps = 0, expansions = 0, backtracks = 0;
        CCLabelBMFont* label = nullptr;
        CCLayerColor* overlay = nullptr; CCLabelBMFont* ovLabel = nullptr; CCLayerColor* ovBar = nullptr;
        rb::Stats stats;
    };
    Fields& fld() { return *m_fields.operator->(); }

    // ================================================================== utilidades base
    float stepDt() { return 1.f / (float)std::max<int64_t>(30, rb::cfg("tps")) * 1.0005f; }
    int blockSteps() { return (int)std::max<int64_t>(1, rb::cfg("block-steps")); }
    bool dual() { return m_player2 && m_player2->isVisible(); }

    void setHold(bool h) {
        auto& f = fld();
        if (f.hold == h) return;
        f.hold = h;
        this->handleButton(h, 1, true);
        if (dual()) this->handleButton(h, 1, false);
    }
    void releaseAll() {
        auto& f = fld();
        this->handleButton(false, 1, true);
        if (dual()) this->handleButton(false, 1, false);
        f.hold = false;
    }
    void stepOnce() {
        auto& f = fld();
        f.inSim = true;
        this->update(stepDt());
        f.inSim = false;
        f.sc++; f.steps++;
    }
    rb::Veh vehicle() {
        auto p = m_player1;
        if (p->m_isDart) return rb::Veh::Wave;
        if (p->m_isShip) return rb::Veh::Ship;
        if (p->m_isBird) return rb::Veh::Ufo;
        if (p->m_isBall) return rb::Veh::Ball;
        if (p->m_isRobot) return rb::Veh::Robot;
        if (p->m_isSpider) return rb::Veh::Spider;
        if (p->m_isSwing) return rb::Veh::Swing;
        return rb::Veh::Cube;
    }
    // Bloque de decision segun vehiculo (solo en modo MAX): vehiculos de precision = bloques mas finos
    int blockFor(rb::Veh v) {
        int b = blockSteps();
        if (!fld().maxMode) return b;
        switch (v) {
            case rb::Veh::Wave: return 1;
            case rb::Veh::Ship: case rb::Veh::Ufo: case rb::Veh::Swing: case rb::Veh::Ball: return std::min(b, 2);
            default: return std::min(b, 3);
        }
    }
    uint64_t stateKey() {
        auto& f = fld();
        auto p = m_player1;
        uint64_t h = (uint64_t)f.sc;
        auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix((uint64_t)(int64_t)std::lround(p->getPositionY() * 2.0));
        mix((uint64_t)(int64_t)std::lround(p->m_yVelocity * 4.0));
        mix((uint64_t)(int64_t)std::lround(p->getPositionX() * 0.25));
        uint64_t b = 0;
        b |= (uint64_t)p->m_isUpsideDown << 0;  b |= (uint64_t)p->m_isOnGround << 1;
        b |= (uint64_t)f.hold << 2;             b |= (uint64_t)p->m_isShip << 3;
        b |= (uint64_t)p->m_isBird << 4;        b |= (uint64_t)p->m_isBall << 5;
        b |= (uint64_t)p->m_isDart << 6;        b |= (uint64_t)p->m_isRobot << 7;
        b |= (uint64_t)p->m_isSpider << 8;      b |= (uint64_t)p->m_isSwing << 9;
        mix(b);
        return h;
    }

    // ================================================================== analisis estatico del nivel
    rb::Stats analyzeLevel(bool buildHitboxIndex) {
        auto& f = fld();
        rb::Stats s;
        if (buildHitboxIndex) f.haz.clear();
        std::vector<std::pair<float, int>> sp;
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            int id = obj->m_objectID;
            float x = obj->getPositionX();
            s.maxX = std::max(s.maxX, x);
            if (obj->m_objectType == GameObjectType::Hazard) {
                s.hazards++;
                if (buildHitboxIndex) {
                    CCRect r = obj->getObjectRect();
                    int b0 = (int)(r.origin.x / 30.f), b1 = (int)((r.origin.x + r.size.width) / 30.f);
                    for (int b = b0; b <= b1; b++) f.haz[b].push_back(r);
                }
            }
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
    // Distancia al hitbox peligroso mas cercano (cuanto mayor, mas seguro el camino). Tope 60.
    float clearance() {
        auto& f = fld();
        CCPoint p = m_player1->getPosition();
        float best = 60.f;
        int b0 = (int)(p.x / 30.f) - 1, b1 = b0 + 5;
        for (int b = b0; b <= b1; b++) {
            auto it = f.haz.find(b);
            if (it == f.haz.end()) continue;
            for (auto& r : it->second) {
                float dx = std::max({r.origin.x - p.x, 0.f, p.x - (r.origin.x + r.size.width)});
                float dy = std::max({r.origin.y - p.y, 0.f, p.y - (r.origin.y + r.size.height)});
                best = std::min(best, std::sqrt(dx * dx + dy * dy));
            }
        }
        return best;
    }

    // ================================================================== trayectoria visible
    int trailIdx() { return (int)Mod::get()->getSavedValue<int64_t>("trail-col", 0); }
    void trailEnsure() {
        auto& f = fld();
        if (f.trailNode && f.trailNode->getParent()) return;
        f.trailNode = CCDrawNode::create();
        m_objectLayer->addChild(f.trailNode, 9999);
    }
    void trailSeg(rb::TrailPt const& a, rb::TrailPt const& b) {
        auto& f = fld();
        int i = trailIdx();
        if (i >= 7) return;
        f.trailNode->drawSegment({a.x, a.y}, {b.x, b.y}, 1.6f, rb::TRAIL_COLORS[i]);
        if (a.hold != b.hold) f.trailNode->drawDot({b.x, b.y}, 3.2f, ccc4f(1.f, 1.f, 1.f, 1.f));   // punto blanco = cambio de input
    }
    void trailAppend() {
        auto& f = fld();
        if (trailIdx() >= 7 || f.trail.size() < 2) return;
        trailEnsure();
        trailSeg(f.trail[f.trail.size() - 2], f.trail.back());
    }
    void trailRedraw() {
        auto& f = fld();
        trailEnsure();
        f.trailNode->clear();
        f.trailNode->setVisible(trailIdx() < 7);
        for (size_t i = 1; i < f.trail.size(); i++) trailSeg(f.trail[i - 1], f.trail[i]);
    }
    void onTrailButton(CCObject*) {
        int i = (trailIdx() + 1) % 8;
        Mod::get()->setSavedValue<int64_t>("trail-col", i);
        trailRedraw();
        Notification::create(fmt::format("Trail: {}", rb::TRAIL_NAMES[i]), NotificationIcon::Info)->show();
    }

    // ================================================================== BUSQUEDA
    void startSearch(bool fresh, bool maxMode) {
        auto& f = fld();
        f.path.clear(); f.route.clear(); f.trail.clear(); f.visited.clear(); f.anchors.clear(); f.commitKeys.clear();
        f.lastHold = false; f.died = f.completed = false; f.commitCount = 0;
        f.steps = f.expansions = f.backtracks = 0; f.bestPct = 0; f.bestSc = 0; f.deaths = 0; f.dStep = 0; f.dObj = -1; f.restarts = 0;
        f.maxMode = maxMode;
        if (fresh) f.replans = 0;
        f.safeA = nullptr; f.safeB = nullptr;
        f.startAnchor = rb::Anchor();
        if (f.trailNode) f.trailNode->clear();
        if (maxMode) f.stats = analyzeLevel(true); else f.haz.clear();
        this->resetLevelFromStart(); releaseAll();
        f.sc = 0; f.t0 = Clock::now();
        f.needInit = true;
        f.mode = rb::Mode::Search;
        showOverlay(maxMode);
        log::info("Route Bot: search started (max={})", maxMode);
        updateHud();
    }

    void runSearchSlice() {
        auto& f = fld();
        if (f.needInit) {
            f.needInit = false;
            rb::Node n; n.cp = this->createCheckpoint(); n.sc = 0; n.prev = false;
            if (!n.cp) { f.mode = rb::Mode::Idle; showOverlay(false); FLAlertLayer::create("Route Bot", "createCheckpoint() returned null - cannot search here.", "OK")->show(); return; }
            auto p = m_player1->getPosition(); n.x = p.x; n.y = p.y; n.key = stateKey();
            f.startAnchor.cp = n.cp; f.startAnchor.sc = 0; f.startAnchor.prev = false; f.startAnchor.lastHold = false;
            f.startAnchor.x = n.x; f.startAnchor.y = n.y; f.startAnchor.key = n.key; f.startAnchor.valid = true;
            f.visited.insert(n.key);
            f.path.push_back(std::move(n));
            return;
        }
        // Presupuesto por frame: lo maximo posible (en MAX hay pantalla de carga, asi que se usa mas tiempo)
        double budget = (double)std::max<int64_t>(2, rb::cfg("budget-ms")) * (f.maxMode ? 3.0 : 1.0);
        budget = std::min(budget, 90.0);
        auto t = Clock::now();
        int n = 0;
        while (f.mode == rb::Mode::Search) {
            if (rb::msSince(f.t0) > (double)rb::cfg("max-seconds") * 1000.0) { finishSearch(false); break; }
            if (!expand()) break;
            if ((++n & 3) == 0 && rb::msSince(t) > budget) break;
        }
    }

    // simula un bloque con la opcion `opt` desde el nodo `n` (restaura estado, pulsa/suelta, avanza D pasos)
    rb::Res runOption(rb::Node& n, bool opt) {
        auto& f = fld();
        rb::Res r;
        setHold(n.prev);                         // dejar el boton como estaba (sobre el estado "muerto")
        this->loadFromCheckpoint(n.cp);
        f.sc = n.sc; f.died = f.completed = false;
        setHold(opt);                            // unico evento real: prev -> opt
        for (int i = 0; i < n.D; i++) {
            stepOnce();
            if (f.died || f.completed) break;
        }
        r.died = f.died; r.completed = f.completed;
        if (!r.died && !r.completed) {
            r.key = stateKey();
            r.clr = f.maxMode ? clearance() : 0.f;
            auto p = m_player1->getPosition(); r.x = p.x; r.y = p.y;
        }
        return r;
    }

    // Primera visita a un nodo: decide el orden de las opciones (segun vehiculo y, en MAX, segun margen de seguridad)
    bool prepareNode(rb::Node& n) {
        auto& f = fld();
        n.init = true;
        setHold(n.prev);
        this->loadFromCheckpoint(n.cp);
        f.sc = n.sc; f.died = f.completed = false;
        rb::Veh v = vehicle();
        n.D = blockFor(v);
        bool a = rb::isTap(v) ? false : n.prev;  // vehiculos de toque: primero soltar; vuelo: mantener
        bool b = !a;
        if (f.restarts >= 2 && std::rand() % 100 < 25) std::swap(a, b);   // tras atascos: variar el orden
        n.ord[0] = a; n.ord[1] = b; n.cnt = 2; n.next = 0;
        if (!f.maxMode) return true;

        rb::Res r[2]; bool valid[2];
        for (int i = 0; i < 2; i++) {
            r[i] = runOption(n, n.ord[i]);
            if (r[i].completed) { n.chosen = n.ord[i]; finishSearch(true); return false; }
            valid[i] = !r[i].died && !f.visited.count(r[i].key);
        }
        float sc[2];
        for (int i = 0; i < 2; i++) sc[i] = r[i].clr + (n.ord[i] == n.prev ? 0.5f : 0.f);
        bool o0 = n.ord[0], o1 = n.ord[1];
        if (valid[0] && valid[1]) {
            if (sc[1] > sc[0] + 0.01f) { n.ord[0] = o1; n.ord[1] = o0; }
            n.cnt = 2;
        } else if (valid[0]) { n.ord[0] = o0; n.cnt = 1; }
        else if (valid[1]) { n.ord[0] = o1; n.cnt = 1; }
        else n.cnt = 0;
        return true;
    }

    size_t windowNow() {
        auto& f = fld();
        size_t W = (size_t)std::max<int64_t>(30, rb::cfg("window"));
        return std::min<size_t>(3000, W * (size_t)(1 + f.restarts / 2));
    }

    void commit(rb::Node& fr) {                  // fija una decision (ya no se puede deshacer)
        auto& f = fld();
        if (f.commitCount % 60 == 0) {           // punto de reinicio seguro cada 60 decisiones
            rb::Anchor a;
            a.cp = fr.cp; a.sc = fr.sc; a.prev = fr.prev; a.lastHold = f.lastHold;
            a.routeN = f.route.size(); a.trailN = f.trail.size(); a.keyN = f.commitKeys.size();
            a.x = fr.x; a.y = fr.y; a.key = fr.key; a.valid = true;
            f.anchors.push_back(a);
            if (f.anchors.size() > 16) f.anchors.pop_front();
        }
        f.commitCount++;
        if (fr.chosen != f.lastHold) { f.route.push_back({fr.sc, fr.chosen}); f.lastHold = fr.chosen; }
        f.trail.push_back({fr.sc, fr.x, fr.y, fr.chosen});
        trailAppend();
        f.commitKeys.push_back({fr.sc, fr.key});
    }

    void restartFrom(rb::Anchor const& a) {
        auto& f = fld();
        f.path.clear();
        f.route.resize(std::min(a.routeN, f.route.size()));
        f.trail.resize(std::min(a.trailN, f.trail.size()));
        f.lastHold = a.lastHold;
        for (size_t i = a.keyN; i < f.commitKeys.size(); i++) f.visited.erase(f.commitKeys[i].second);   // lo que era "vivo" vuelve a ser explorable
        f.commitKeys.resize(std::min(a.keyN, f.commitKeys.size()));
        trailRedraw();
        rb::Node n; n.cp = a.cp; n.sc = a.sc; n.prev = a.prev; n.x = a.x; n.y = a.y; n.key = a.key;
        f.path.push_back(std::move(n));
        f.died = f.completed = false;
    }

    // Se agotaron todas las opciones: NO se rinde, vuelve a un punto anterior y prueba otros caminos
    bool onExhausted() {
        auto& f = fld();
        f.restarts++;
        if (f.restarts > (int)rb::cfg("max-replans") * 20 + 60) { finishSearch(false); return false; }
        if (!f.anchors.empty()) { rb::Anchor a = f.anchors.back(); f.anchors.pop_back(); restartFrom(a); return true; }
        if (f.startAnchor.valid) { restartFrom(f.startAnchor); return true; }
        finishSearch(false);
        return false;
    }

    // Una expansion de la busqueda en profundidad. Devuelve false si la busqueda termino.
    bool expand() {
        auto& f = fld();
        if (f.path.empty()) return onExhausted();
        rb::Node* np = &f.path.back();
        if (!np->init) { if (!prepareNode(*np)) return false; np = &f.path.back(); }
        rb::Node& n = *np;
        if (n.next >= n.cnt) {                   // sin salida -> retroceder (la muerte NO detiene la busqueda)
            f.path.pop_back(); f.backtracks++;
            if (f.path.empty()) return onExhausted();
            return true;
        }
        bool opt = n.ord[n.next++];
        n.chosen = opt;
        rb::Res r = runOption(n, opt);
        f.expansions++;
        if (r.completed) { finishSearch(true); return false; }
        if (r.died) return true;
        if (!f.visited.insert(r.key).second) return true;      // estado repetido -> podar

        rb::Node c; c.cp = this->createCheckpoint(); c.sc = f.sc; c.prev = opt; c.x = r.x; c.y = r.y; c.key = r.key;
        f.path.push_back(std::move(c));
        f.bestPct = std::max(f.bestPct, (double)this->getCurrentPercent());
        f.bestSc = std::max(f.bestSc, f.sc);

        size_t W = windowNow(
