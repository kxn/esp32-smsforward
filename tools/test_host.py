"""Compile the firmware's portable regression tests and run each in a fresh directory."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware" / "main"
CASES = {
    ROOT / "tools/test_mms_policy.c": ["mms_policy.c"],
    ROOT / "tools/test_mms_fetch.c": [],
    ROOT / "tools/test_mms_path.c": ["pdu.c","wap_mms.c","mms_policy.c"],
    ROOT / "tools/test_sms_pdu.c": ["pdu.c"],
    ROOT / "tools/test_wap_mms.c": ["wap_mms.c"],
    ROOT / "tools/test_telemetry.c": ["telemetry.c"],
    ROOT / "tools/test_factory_reset.c": ["factory_reset.c"],
    ROOT / "tools/test_firmware_update.c": ["firmware_update.c"],
    ROOT / "tools/test_ble_lifecycle.c": ["ble_lifecycle_policy.c"],
    ROOT / "tools/test_sms_assembly.c": ["sms_assembly.c"],
    ROOT / "tools/test_sms_seen.c": ["sms_seen.c"],
    ROOT / "tools/test_sms_text.c": ["sms_text.c"],
    ROOT / "tools/test_sms_send_capacity.c": ["pdu.c", "sms_text.c"],
    ROOT / "tools/test_sms_time_policy.c": [],
    ROOT / "tools/test_modem_transaction.c": [],
    ROOT / "tools/test_modem_transaction_flow.c": [],
    MAIN / "tests/test_feishu_cmd_policy.c": [],
    MAIN / "tests/test_feishu_media_policy.c": [],
    MAIN / "tests/test_cmd_call_notice.c": ["phone_region.c"],
    ROOT / "tools/test_phone_region.c": ["phone_region.c"],
    MAIN / "tests/test_cmd_policy_cereg.c": [],
    MAIN / "tests/test_sim_number_policy.c": [],
    MAIN / "tests/test_feishu_reply_retry.c": [],
    MAIN / "tests/test_feishu_config_policy.c": [],
    MAIN / "tests/test_feishu_pbbp2.c": ["pbbp2.c"],
    MAIN / "tests/test_feishu_map_store.c": ["map_store.c"],
    MAIN / "tests/test_feishu_framing.c": ["feishu_proto.c"],
}
EXTRA_INCLUDES = {
    ROOT / "tools/test_factory_reset.c": [ROOT / "tools/modem_fake"],
    ROOT / "tools/test_firmware_update.c": [ROOT / "tools/firmware_fake"],
    MAIN / "tests/test_feishu_media_policy.c": [ROOT / "tools/modem_fake"],
    ROOT / "tools/test_modem_transaction_flow.c": [ROOT / "tools/modem_fake"],
    ROOT / "tools/test_mms_fetch.c": [ROOT / "tools/modem_fake"],
}


def main():
    compiler = shutil.which(os.environ.get("CC", "gcc"))
    if not compiler:
        raise SystemExit("Set CC to a host C compiler.")
    with tempfile.TemporaryDirectory(prefix="smsf-tests-") as folder:
        (Path(folder) / "tools").mkdir()
        for test, modules in CASES.items():
            executable = Path(folder) / (test.stem + (".exe" if os.name == "nt" else ""))
            subprocess.run([compiler, "-std=gnu17", "-Wall", "-Wextra", "-I", "firmware/main",
                            *(arg for folder in EXTRA_INCLUDES.get(test, []) for arg in ("-I", folder.as_posix())),
                            test.relative_to(ROOT).as_posix(),
                            *((MAIN / name).relative_to(ROOT).as_posix() for name in modules),
                            "-o", executable.as_posix()], check=True, cwd=ROOT)
            subprocess.run([executable], check=True, cwd=folder)
    print(f"{len(CASES)} test programs passed")


if __name__ == "__main__":
    main()
