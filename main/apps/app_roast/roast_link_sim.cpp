/*
 * Simulated roaster for practising the UI without the machine.
 * Same register semantics as the BLE link. Thermal model and on-board PID fitted on R0012-R0014 (roast_control.h:
 * PlantModel): heater -> inlet air after a 5 s dead time and a 1 s lag, beans approach the air at 0.0175/s at
 * 53.5 % fan; more airflow dilutes the air and speeds up the transfer. The on-board PID is the fitted positional
 * form heat = 0.50 e + 0.036 integral(e dt) + 12 %.
 */
#include "roast_link.h"
#include <esp_timer.h>
#include <cmath>
#include <cstdlib>
#include <map>

namespace roast {

class SimLink : public RoasterLink {
public:
    bool begin() override
    {
        _t0 = _last = now();
        _state      = LinkState::Ready;
        return true;
    }
    void end() override
    {
        _state = LinkState::Off;
    }
    LinkState state() override
    {
        return _state;
    }
    std::string deviceName() override
    {
        return "Simulator";
    }
    std::string lastError() override
    {
        return "";
    }
    std::vector<ScanEntry> scanResults() override
    {
        return {{"Simulator", -40}};
    }
    bool isSimulated() const override
    {
        return true;
    }

    RegSnapshot snapshot() override
    {
        step();
        RegSnapshot s;
        s.reg[2]  = _running && _timerOn ? (int16_t)_timer : 0;
        s.reg[4]  = (int16_t)lround(_bt * 10);
        s.reg[9]  = (int16_t)_target;
        s.reg[11] = (int16_t)lround(_heat * 10);
        s.reg[12] = (int16_t)_fan;
        s.reg[13] = _running ? 1 : 0;
        s.reg[15] = _pid ? 1 : 0;
        s.reg[16] = _running ? (int16_t)_fan : 0;
        s.reg[20] = 1000;
        s.reg[21] = 6;
        s.reg[22] = 1;
        s.reg[23] = 8;
        s.reg[24] = 900;
        s.reg[25] = 2000;
        s.reg[26] = 100;
        s.reg[28] = 500;
        s.reg[29] = 1000;
        s.valid   = 0x3FFFFFFC;
        s.lastRxMs = (uint32_t)(now() * 1000);
        s.frames   = (uint32_t)((now() - _t0) * 2);
        return s;
    }

    uint32_t command(const std::vector<RegWrite>& writes) override
    {
        for (auto& w : writes) apply(w.first, w.second);
        uint32_t id = _next++;
        _tx[id]     = TxStatus::Ok;
        return id;
    }
    TxStatus txStatus(uint32_t id) override
    {
        auto it = _tx.find(id);
        return it == _tx.end() ? TxStatus::Failed : it->second;
    }
    void setTarget(uint16_t raw) override
    {
        _target = raw;
    }
    void setHeat(uint16_t raw) override
    {
        apply(11, raw);
    }
    void clearQueue() override
    {
    }

private:
    LinkState _state = LinkState::Off;
    double _t0 = 0, _last = 0;
    double _bt = 26.0, _air = 26.0, _heat = 0, _integ = 0, _timer = 0;
    double _hist[64] = {};  // heater output per 0.1 s slot, for the dead time
    double _histT    = 0;
    int _histPos     = 0;
    int _target = 500, _fan = 530;
    bool _running = false, _pid = false, _timerOn = false, _dropped = false;
    uint32_t _next = 1;
    std::map<uint32_t, TxStatus> _tx;

    static double now()
    {
        return esp_timer_get_time() / 1e6;
    }

    void apply(uint16_t a, uint16_t v)
    {
        switch (a) {
            case 2:
                _timer   = 0;
                _timerOn = true;
                _dropped = false;
                break;
            case 9:
                _target = v;
                break;
            case 11:
                if (!_pid) _heat = v / 10.0;
                break;
            case 12:
                _fan = v;
                break;
            case 13:
                if (v == 1) {
                    _running = true;
                    _heat    = 0;
                }
                break;
            case 14:
                if (v == 2) {  // drop: heater off, keep blowing to cool
                    _pid     = false;
                    _heat    = 0;
                    _timerOn = false;
                    _dropped = true;
                } else if (v == 1) {  // stop
                    _running = false;
                    _pid     = false;
                    _heat    = 0;
                    _timerOn = false;
                }
                break;
            case 15:
                _pid   = v == 1;
                _integ = 0;
                break;
            default:
                break;
        }
    }

    void step()
    {
        double t  = now();
        double dt = t - _last;
        if (dt <= 0) return;
        if (dt > 2) dt = 2;
        _last = t;
        if (_timerOn) _timer += dt;

        double fanFrac = _running ? _fan / 1000.0 : 0;
        if (_pid && _running) {  // fitted on-board PID (positional, 1 s cycle)
            double err = _target / 10.0 - _bt;
            _integ += 0.036 * err * dt;
            if (_integ > 88) _integ = 88;
            if (_integ < -12) _integ = -12;
            double out = 0.50 * err + _integ + 12;
            _heat      = out < 0 ? 0 : (out > 100 ? 100 : out);
        }
        // Dead time: the air sees the heater output of ~5 s ago.
        _histT += dt;
        while (_histT >= 0.1) {
            _histT -= 0.1;
            _hist[_histPos] = _heat;
            _histPos        = (_histPos + 1) % 64;
        }
        double late = _hist[(_histPos + 64 - 50) % 64];
        double f    = fanFrac > 0.2 ? fanFrac : 0.2;
        double airTarget = 26 + (_running ? late * 2.2 * (0.535 / f) : 0);
        _air += (airTarget - _air) * (1 - exp(-dt / 1.0));
        double k = _running ? 0.0175 * (fanFrac / 0.535) : 0.002;
        double endo = (_bt > 196 && _bt < 203 && _timerOn) ? 0.05 : 0;  // first crack: a small heat sink (assumed)
        _bt += ((_air - _bt) * k - endo) * dt;
        _bt += ((rand() % 100) - 50) / 2000.0;
        if (_bt < 20) _bt = 20;
    }
};

RoasterLink* CreateSimLink()
{
    return new SimLink();
}

}  // namespace roast
