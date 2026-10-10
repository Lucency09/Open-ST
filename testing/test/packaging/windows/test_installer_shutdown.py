"""只操作本脚本创建的测试进程；验证安装助手身份边界、正常退出与超时兜底。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


class InstallerShutdownTests(unittest.TestCase):
    """测试产物全部位于仓库testoutput，不运行Setup或真实Open-ST。"""

    def setUp(self):
        root = Path(__file__).resolve().parents[3] / "testoutput"
        root.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="installer-shutdown-", dir=root)
        self.root = Path(self.temporary.name)
        self.children = []

    def tearDown(self):
        # 仅清理本测试Popen持有的子进程，不扫描或按名称关闭任何用户进程。
        for child in self.children:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
        self.temporary.cleanup()

    def start_fixture(self, name="目标 空格", mode="normal"):
        directory = self.root / name
        directory.mkdir()
        executable = directory / "Open-ST.exe"
        shutil.copyfile(PROBE, executable)
        ready = directory / "ready"
        closed = directory / "closed"
        child = subprocess.Popen([str(executable), mode, str(ready), str(closed)])
        self.children.append(child)
        deadline = time.monotonic() + 5
        while not ready.exists():
            self.assertIsNone(child.poll(), "隔离测试子进程提前退出")
            self.assertLess(time.monotonic(), deadline, "隔离测试子进程启动超时")
            time.sleep(0.02)
        return directory, child, closed

    def invoke(self, directory):
        result = subprocess.run([str(HELPER), "--directory", str(directory)], timeout=25, check=False)
        self.assertNotIn(result.returncode, (11, 12),
                         "无法验证当前交互桌面身份或权限；请在原Windows交互用户下运行此专项")
        return result.returncode

    def test_absent_executable_is_noop(self):
        self.assertEqual(self.invoke(self.root), 0)

    def test_relative_directory_is_rejected(self):
        self.assertEqual(self.invoke(Path("relative")), 10)

    def test_argument_contract_is_closed(self):
        result = subprocess.run([str(HELPER)], timeout=5, check=False)
        self.assertEqual(result.returncode, 10)

    def test_existing_v1_message_window_closes_gracefully(self):
        directory, child, closed = self.start_fixture()
        self.assertEqual(self.invoke(directory), 0)
        self.assertEqual(child.wait(timeout=2), 0)
        self.assertTrue(closed.exists(), "必须走正常WM_CLOSE收尾")

    def test_other_portable_copy_remains_running(self):
        other, foreign, foreign_closed = self.start_fixture("other portable")
        directory, child, closed = self.start_fixture()
        self.assertNotEqual(other, directory)
        self.assertEqual(self.invoke(directory), 0)
        self.assertEqual(child.wait(timeout=2), 0)
        self.assertTrue(closed.exists())
        self.assertIsNone(foreign.poll())
        self.assertFalse(foreign_closed.exists())

    def test_unresponsive_instance_is_forced_after_grace_period(self):
        directory, child, closed = self.start_fixture(mode="ignore")
        start = time.monotonic()
        self.assertEqual(self.invoke(directory), 0)
        self.assertGreaterEqual(time.monotonic() - start, 14.5)
        self.assertNotEqual(child.wait(timeout=2), 0)
        self.assertFalse(closed.exists(), "子进程未执行正常关闭标记，必须是超时兜底")

    def test_hardlinked_target_is_rejected_without_closing(self):
        directory, child, closed = self.start_fixture()
        os.link(directory / "Open-ST.exe", self.root / "alias.exe")
        self.assertEqual(self.invoke(directory), 10)
        self.assertIsNone(child.poll())
        self.assertFalse(closed.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    args, remainder = parser.parse_known_args()
    HELPER, PROBE = args.helper.resolve(strict=True), args.probe.resolve(strict=True)
    unittest.main(argv=[sys.argv[0], *remainder])
