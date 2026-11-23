"""Browser smoke test for the SMSF console with a deterministic BLE device stub."""
from __future__ import annotations

import threading
import json
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import gettempdir

from playwright.sync_api import sync_playwright


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"

BLE_STUB = r"""
(() => {
  const state = window.mockBle = {
    failNext: null,
    telemetryCount: 0,
    telemetryDelayMs: 0,
    telemetryUnsupported: false,
    slots: [
      { n: 1, name: '主卡', num: '13800138000', reg: true, op: '中国移动', csq: 23 },
      { n: 2, name: '备用卡', num: '卡2-123456', reg: false, op: '', csq: null }
    ],
    wifi: { ssid: 'Studio', cfg_ssid: 'Studio', ip: '192.168.1.40', cfg_ip: '192.168.1.40',
      mask: '255.255.255.0', gw: '192.168.1.1', dns: '223.5.5.5', dhcp: false, rssi: -51, up: 7200 },
    fs: { state: 'offline', configured: true, app_id: 'cli_initial' },
    tz: 8
  };
  class Char extends EventTarget {
    constructor(notify) { super(); this.notify = notify; this.value = null; this.bytes = []; }
    async startNotifications() { return this; }
    async writeValueWithResponse(part) {
      for (const b of part) {
        if (b === 10) {
          const req = JSON.parse(new TextDecoder().decode(new Uint8Array(this.bytes)));
          this.bytes = [];
          this.respond(req);
        } else this.bytes.push(b);
      }
    }
    respond(req) {
      let reply;
      if (state.failNext) { const err = state.failNext; state.failNext = null; reply = { id: req.id, ok: false, err }; }
      else if (req.cmd === 'status') reply = { id: req.id, ok: true, data: {
        slots: state.slots, wifi: state.wifi, feishu: state.fs,
        time: { synced: true, utc: 1763900000, tz: state.tz }
      }};
      else if (req.cmd === 'telemetry') {
        const count = ++state.telemetryCount;
        if (state.telemetryUnsupported) reply = { id: req.id, ok: false, err: 'unknown' };
        else reply = { id: req.id, ok: true, data: {
          cpu: count === 1 ? { state: 'warming_up', used_pct: null, window_ms: null }
            : { state: 'ok', used_pct: 20 + count, core_pct: [15, 30], window_ms: 5000 },
          temperature_c: 41.2, temperature_state: 'ok', heap_free_bytes: 327680,
          heap_total_bytes: 524288, psram_free_bytes: 6291456, psram_total_bytes: 8388608,
          uptime_s: 3661
        }};
        if (state.telemetryDelayMs) {
          setTimeout(() => this.deliver(reply), state.telemetryDelayMs);
          return;
        }
      }
      else if (req.cmd === 'wifi_set') {
        state.wifi = { ...state.wifi, cfg_ssid: req.ssid, ssid: req.ssid, dhcp: req.dhcp,
          ip: req.dhcp ? '192.168.1.88' : req.ip, cfg_ip: req.ip, mask: req.mask, gw: req.gw, dns: req.dns };
        reply = { id: req.id, ok: true };
      } else if (req.cmd === 'feishu_set') {
        state.fs = { ...state.fs, configured: true, app_id: req.app_id || state.fs.app_id };
        reply = { id: req.id, ok: true };
      } else if (req.cmd === 'slot_set') {
        state.slots[req.slot - 1] = { ...state.slots[req.slot - 1], name: req.name, num: req.number };
        reply = { id: req.id, ok: true };
      } else if (req.cmd === 'tz_set') { state.tz = req.offset; reply = { id: req.id, ok: true }; }
      else if (req.cmd === 'wifi_scan') reply = { id: req.id, ok: true, data: [{ ssid: 'Studio', rssi: -51 }] };
      else reply = { id: req.id, ok: true };
      this.deliver(reply);
    }
    deliver(reply) {
      const raw = new TextEncoder().encode(JSON.stringify(reply) + '\n');
      // Small chunks deliberately split UTF-8 characters and JSON tokens.
      for (let i = 0; i < raw.length; i += 11) {
        const view = new DataView(raw.slice(i, i + 11).buffer);
        const ev = new Event('characteristicvaluechanged');
        Object.defineProperty(ev, 'target', { value: { value: view } });
        this.notify.dispatchEvent(ev);
      }
    }
  }
  const rsp = new Char(null);
  rsp.notify = rsp;
  const cmd = new Char(rsp);
  const service = { async getCharacteristic(uuid) { return uuid.includes('0002') ? cmd : rsp; } };
  const device = new EventTarget();
  device.name = 'SMSF-1234';
  device.gatt = {
    connected: false,
    async connect() { this.connected = true; return { async getPrimaryService() { return service; } }; },
    disconnect() { this.connected = false; device.dispatchEvent(new Event('gattserverdisconnected')); }
  };
  Object.defineProperty(navigator, 'bluetooth', { configurable: true, value: {
    async requestDevice(opts) { state.request = opts; return device; }
  }});
})();
"""


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB), **kwargs)

    def log_message(self, *_args):
        pass


class LanHandler(Handler):
    def do_GET(self):
        if self.path == "/api/info":
            self._json(200, {"transport": "lan"})
            return
        super().do_GET()

    def do_POST(self):
        if self.path != "/api/rpc":
            self._json(404, {"ok": False, "err": "not_found"})
            return
        if not self.server.available:
            self._json(503, {"ok": False, "err": "offline"})
            return
        auth = self.headers.get("Authorization", "")
        self.server.auth_headers.append(auth)
        if auth != "Bearer " + self.server.expected_token:
            self._json(401, {"ok": False, "err": "unauthorized"})
            return
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length) or b"{}")
        if request.get("cmd") == "telemetry":
            self.server.telemetry_count += 1
            self._json(200, {"id": request.get("id"), "ok": True, "data": {
                "cpu": {"state": "ok", "used_pct": 26.7, "window_ms": 5000},
                "temperature_c": None, "temperature_state": "unsupported",
                "heap_free_bytes": 204800, "heap_total_bytes": 524288, "uptime_s": 120,
            }})
            return
        if request.get("cmd") == "factory_reset":
            self.server.factory_requests.append(request)
            confirmed = request.get("confirm") is True
            response = {"id": request.get("id"), "ok": confirmed}
            if not confirmed:
                response["err"] = "confirm_required"
            self._json(200, response)
            return
        if request.get("cmd") != "status":
            self._json(200, {"id": request.get("id"), "ok": True})
            return
        self._json(200, {"id": request.get("id"), "ok": True, "data": {
            "slots": [
                {"n": 1, "name": "LAN 主卡", "num": "13800138000", "reg": True, "op": "运营商", "csq": 20},
                {"n": 2, "name": "LAN 副卡", "num": "13900139000", "reg": False, "op": "", "csq": 0},
            ],
            "wifi": {"ssid": "Studio", "cfg_ssid": "Studio", "ip": "192.168.1.40", "cfg_ip": "192.168.1.40", "dhcp": True},
            "feishu": {"state": "online", "configured": True, "app_id": "cli_test"},
            "time": {"synced": True, "utc": 1763900000, "tz": 8},
        }})

    def _json(self, status, body):
        raw = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def start_lan_server(token):
    server = ThreadingHTTPServer(("127.0.0.1", 0), LanHandler)
    server.expected_token = token
    server.auth_headers = []
    server.telemetry_count = 0
    server.factory_requests = []
    server.available = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def test_lan_mode(browser) -> None:
    token = "a" * 32
    other_token = "b" * 32
    first = start_lan_server(token)
    second = start_lan_server(other_token)
    ble = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=ble.serve_forever, daemon=True).start()
    page = browser.new_page(viewport={"width": 1280, "height": 900})
    try:
        first_url = f"http://127.0.0.1:{first.server_port}/index.html#token={token}"
        page.goto(first_url)
        page.locator("#view-overview").wait_for(state="visible")
        assert page.get_by_text("LAN 主卡", exact=True).is_visible()
        assert page.locator("#lanGate").is_hidden()
        assert page.locator("#btnConnect").is_hidden()
        assert page.locator("#setupGuide").is_hidden()
        if "token=" in page.url:
            raise AssertionError("management token remained in URL")
        assert page.evaluate("sessionStorage.length") == 1
        page.get_by_text("26.7%", exact=True).wait_for()
        assert page.locator("#temperatureDetail").inner_text() == "该设备未提供传感器"
        assert first.telemetry_count == 1
        if not first.auth_headers or not all(h == "Bearer " + token for h in first.auth_headers):
            raise AssertionError("LAN authorization header mismatch")

        # Leaving the overview aborts an in-flight LAN monitor fetch.
        page.evaluate("""() => {
            const original = window.fetch;
            window.delayedTelemetry = 0;
            window.cancelledTelemetry = 0;
            window.fetch = (url, options) => {
                if (url !== '/api/rpc' || JSON.parse(options.body).cmd !== 'telemetry')
                    return original(url, options);
                ++window.delayedTelemetry;
                return new Promise((resolve, reject) => {
                    options.signal.addEventListener('abort', () => {
                        ++window.cancelledTelemetry;
                        reject(new DOMException('Aborted', 'AbortError'));
                    }, { once: true });
                });
            };
        }""")
        page.get_by_role("button", name="网络", exact=True).click()
        page.get_by_role("button", name="设备概览", exact=True).click()
        page.wait_for_function("window.delayedTelemetry === 1")
        page.get_by_role("button", name="网络", exact=True).click()
        page.wait_for_function("window.cancelledTelemetry === 1")

        # The token survives a same-tab reload only in this origin's session storage.
        page.reload()
        page.locator("#view-overview").wait_for(state="visible")
        if "token=" in page.url:
            raise AssertionError("management token remained after reload")
        if first.auth_headers[-1] != "Bearer " + token:
            raise AssertionError("LAN reload did not reuse same-tab credential")

        first.available = False
        page.evaluate("refresh()")
        page.get_by_text("局域网暂不可达", exact=True).wait_for()
        first.available = True
        page.evaluate("refresh()")
        page.get_by_text("局域网已连接", exact=True).wait_for()

        # A different device address/origin gets no credential from this tab.
        second_url = f"http://127.0.0.1:{second.server_port}/index.html"
        page.goto(second_url)
        page.locator("#lanGate").wait_for(state="visible")
        assert "飞书私聊发送 /web" in page.locator("#lanGateMessage").inner_text()
        assert page.locator("main").is_hidden()
        assert not second.auth_headers

        # A self-hosted BLE page is not promoted to LAN mode by a fragment alone.
        page.goto(f"http://127.0.0.1:{ble.server_port}/index.html#token={token}")
        page.get_by_role("button", name="连接设备").wait_for(state="visible")
        assert page.locator("#lanGate").is_hidden()
        if "token=" in page.url:
            raise AssertionError("token fragment remained on self-hosted BLE page")

        # Expired credentials are removed and the page returns to the link gate.
        page.goto(first_url)
        page.locator("#view-overview").wait_for(state="visible")
        first.expected_token = other_token
        page.reload()
        page.locator("#lanGate").wait_for(state="visible")
        assert "授权已失效" in page.locator("#lanGateMessage").inner_text()
        assert page.evaluate("sessionStorage.length") == 0
        if "token=" in page.url:
            raise AssertionError("expired management token remained in URL")
        assert page.locator("#btnFactory").is_hidden()

        first.expected_token = token
        page.goto(first_url)
        page.locator("#view-overview").wait_for(state="visible")
        test_factory_reset(page, first)
        print("PASS LAN detection, fragment cleanup, Bearer RPC, telemetry cancellation, token isolation, and factory reset confirmation")
    finally:
        page.close()
        for server in (first, second, ble):
            server.shutdown()
            server.server_close()


def test_factory_reset(page, server) -> None:
    page.get_by_role("button", name="系统", exact=True).click()
    reset = page.get_by_role("button", name="恢复出厂设置", exact=True)
    dialog = page.get_by_role("dialog", name="恢复出厂设置？")
    cancel = dialog.get_by_role("button", name="取消", exact=True)

    reset.click()
    assert dialog.is_visible()
    assert cancel.evaluate("el => el === document.activeElement")
    page.keyboard.press("Enter")
    assert dialog.is_hidden()

    reset.click()
    page.keyboard.press("Escape")
    assert dialog.is_hidden()

    reset.click()
    cancel.click()
    assert dialog.is_hidden()
    assert not server.factory_requests

    reset.click()
    dialog.get_by_role("button", name="确认恢复出厂", exact=True).evaluate("""button => {
        const form = button.form;
        button.click();
        form.requestSubmit(button);
    }""")
    page.get_by_text("设备正在重启。请打开蓝牙配置页重新配网。", exact=True).wait_for()
    assert len(server.factory_requests) == 1
    assert server.factory_requests[0]["confirm"] is True
    assert page.evaluate("sessionStorage.length") == 0


def test_ble_setup_guide(page) -> None:
    title = page.locator("#setupTitle")
    guidance = page.locator("#setupText")
    assert title.inner_text() == "等待飞书上线"
    assert "自动关闭蓝牙" in guidance.inner_text()
    page.evaluate("""() => {
        const {wifi, fs} = window.mockBle;
        window.savedWifi = {...wifi};
        window.savedFs = {...fs};
        wifi.ip = '';
        return refresh();
    }""")
    assert title.inner_text() == "连接 Wi-Fi"
    assert "保存并连接" in guidance.inner_text()
    page.evaluate("window.mockBle.wifi = window.savedWifi; window.mockBle.fs.configured = false; refresh()")
    assert title.inner_text() == "配置飞书"
    assert "保存飞书配置" in guidance.inner_text()
    page.evaluate("window.mockBle.fs.configured = true; window.mockBle.fs.state = 'online'; refresh()")
    assert title.inner_text() == "接下来使用局域网管理"
    assert "/web" in guidance.inner_text()
    page.evaluate("window.mockBle.fs = window.savedFs; refresh()")


def test_ble_monitor(page) -> None:
    page.get_by_text("等待首个完整采样窗口", exact=True).wait_for()
    assert page.locator("#temperatureValue").inner_text() == "41.2 °C"
    assert page.locator("#heapValue").inner_text() == "320 KiB / 512 KiB"
    assert page.locator("#uptimeValue").inner_text() == "1 小时 1 分"
    def count():
        return page.evaluate("window.mockBle.telemetryCount")

    assert count() == 1
    page.clock.run_for(5100)
    assert count() == 2
    assert page.locator("#cpuValue").inner_text() == "22.0%"
    assert "5 秒采样窗口" in page.locator("#cpuDetail").inner_text()

    # A slow reply cannot start a second poll, and leaving the view discards it.
    page.evaluate("window.mockBle.telemetryDelayMs = 7500")
    page.clock.run_for(10200)
    assert count() == 3
    page.get_by_role("button", name="网络", exact=True).click()
    page.clock.run_for(9000)
    assert count() == 3
    assert page.locator("#cpuValue").text_content() == "22.0%"

    # Resuming is immediate and a hidden document stops the monitoring timer.
    page.evaluate("window.mockBle.telemetryDelayMs = 0")
    page.get_by_role("button", name="设备概览", exact=True).click()
    page.get_by_text("24.0%", exact=True).wait_for()
    page.evaluate("""() => {
        Object.defineProperty(document, 'hidden', { configurable: true, value: true });
        document.dispatchEvent(new Event('visibilitychange'));
    }""")
    page.clock.run_for(15000)
    assert count() == 4
    page.evaluate("""() => {
        delete document.hidden;
        document.dispatchEvent(new Event('visibilitychange'));
    }""")
    page.get_by_text("25.0%", exact=True).wait_for()
    assert count() == 5


def main() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            page = browser.new_page(viewport={"width": 1440, "height": 1000})
            page.clock.install()
            page.add_init_script(BLE_STUB)
            page.goto(f"http://127.0.0.1:{server.server_port}/index.html")

            assert page.get_by_role("heading", name="设备概览").is_visible()
            assert page.locator("#heading-network").count() == 1
            assert page.locator("#view-network").is_hidden()
            assert page.locator("#btnSaveWifi").is_disabled()
            assert "USB 控制台" in page.locator("#setupText").inner_text()

            page.get_by_role("button", name="连接设备").click()
            page.get_by_text("主卡", exact=True).wait_for()
            assert page.evaluate("window.mockBle.request.optionalServices[0]") == "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
            assert "中国移动" in page.locator("#sim0").inner_text()
            assert "号码未知" not in page.locator("#sim0").inner_text()
            assert "未注册" in page.locator("#sim1").inner_text()
            assert "192.168.1.40" in page.locator("#networkGrid").inner_text()
            assert page.locator("#view-overview").is_visible()
            assert page.locator("#view-network").is_hidden()
            test_ble_monitor(page)
            test_ble_setup_guide(page)

            page.get_by_role("button", name="网络", exact=True).click()
            assert page.locator("#view-network").is_visible()
            page.locator("#ssid").fill("Studio-5G")
            page.locator("#pass").fill("example-secret")
            page.evaluate("window.mockBle.failNext='save_failed'")
            page.get_by_role("button", name="保存并连接").click()
            page.get_by_text("save_failed", exact=True).wait_for()
            page.evaluate("refresh()")
            assert page.locator("#pass").input_value() == "example-secret"
            assert page.locator("#ssid").input_value() == "Studio-5G"

            page.get_by_role("button", name="保存并连接").click()
            page.get_by_text("网络配置已保存", exact=False).wait_for()
            assert page.locator("#pass").input_value() == ""
            page.wait_for_function("document.querySelector('#networkGrid').textContent.includes('Studio-5G')")

            page.get_by_role("button", name="飞书", exact=True).click()
            page.locator("#appsecret").fill("write-only-secret")
            page.get_by_role("button", name="保存飞书配置").click()
            page.get_by_text("飞书配置已保存", exact=False).wait_for()
            assert page.locator("#appsecret").input_value() == ""
            assert "write-only-secret" not in page.locator("body").inner_text()

            page.get_by_role("button", name="卡槽", exact=True).click()
            assert page.locator("#s1num").input_value() == ""
            page.locator("#s1name").fill("工作卡")
            page.get_by_role("button", name="保存卡槽").click()
            page.get_by_text("卡槽配置已保存", exact=False).wait_for()
            page.get_by_role("button", name="系统", exact=True).click()
            assert page.locator("#btnFactory").is_hidden()
            assert page.locator("#factoryLanOnly").is_visible()
            page.locator("#tz").fill("9")
            page.get_by_role("button", name="保存时区").click()
            page.get_by_text("时区已保存", exact=False).wait_for()

            desktop = Path(gettempdir()) / "smsf-console-desktop.png"
            page.get_by_role("button", name="设备概览", exact=True).click()
            page.locator("#toast").evaluate("el => { el.className = ''; el.style.display = 'none'; }")
            page.screenshot(path=str(desktop), full_page=True)
            page.locator("#toast").evaluate("el => el.style.display = ''")
            page.get_by_role("button", name="断开连接").click()
            page.locator("#toast").get_by_text("连接已断开", exact=True).wait_for()
            assert page.locator("#btnSaveWifi").is_disabled()
            assert "设备未连接" in page.locator("#sim0").inner_text()
            page.get_by_role("button", name="连接设备").click()
            page.wait_for_function("document.querySelector('#sim0').textContent.includes('工作卡')")

            for width in (390, 360, 320):
                page.set_viewport_size({"width": width, "height": 844})
                assert page.locator("body").evaluate("el => el.scrollWidth <= innerWidth"), f"horizontal overflow at {width}px"
            mobile = Path(gettempdir()) / "smsf-console-mobile.png"
            page.locator("#toast").evaluate("el => { el.className = ''; el.style.display = 'none'; }")
            page.screenshot(path=str(mobile), full_page=True)
            assert page.get_by_role("button", name="设备概览", exact=True).get_attribute("aria-current") == "page"
            page.evaluate("window.mockBle.telemetryUnsupported = true")
            page.clock.run_for(5100)
            assert page.locator("#monitorStatus").inner_text() == "当前固件不提供运行监控"
            unsupported_count = page.evaluate("window.mockBle.telemetryCount")
            page.clock.run_for(15000)
            assert page.evaluate("window.mockBle.telemetryCount") == unsupported_count
            print("PASS: BLE and LAN monitoring, BLE polling cadence, visibility, request serialization, status, settings, reconnect, mobile widths")
            print(f"Screenshots: {desktop} ; {mobile}")
            test_lan_mode(browser)
            browser.close()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
