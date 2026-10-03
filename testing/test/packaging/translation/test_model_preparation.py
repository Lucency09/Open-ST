"""仅用小型合成资产验证准备锁、整包发布与失败保留；不转换或下载真实模型。"""
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest import mock

# 直接导入生产准备入口用于测试，但不在源码树写入 Python 字节码。
sys.dont_write_bytecode = True
REPOSITORY = pathlib.Path(__file__).resolve().parents[4]
SPEC = importlib.util.spec_from_file_location('model_preparation', REPOSITORY / 'packaging/translation/convert_models.py')
PREPARATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARATION)
CMAKE = PREPARATION.find_cmake(os.environ.get('OPEN_ST_TEST_CMAKE'))
OUTPUT = REPOSITORY / 'testing/testoutput/model-preparation'
OUTPUT.mkdir(parents=True, exist_ok=True)
NAMES = ('config.json', 'model.bin', 'shared_vocabulary.json', 'source.spm', 'target.spm', 'LICENSE', 'README.md')


class ModelPreparationTest(unittest.TestCase):
    # 每例只有少量合成字节，CMake 及转换入口使用真实实现。
    # 入参：无。返回：独占临时根。
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='合成 模型 ', dir=OUTPUT)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.sources = tempfile.TemporaryDirectory(prefix='sources-', dir=OUTPUT)
        (self.root / 'cmake').mkdir()
        (self.root / 'packaging/translation').mkdir(parents=True)
        for name in ('OpenSTModelCache.cmake', 'OpenSTTranslationModels.cmake'):
            shutil.copyfile(REPOSITORY / 'cmake' / name, self.root / 'cmake' / name)
        self.cache = self.root / '.cache/translation-models'
        self.cache.mkdir(parents=True)
        self.contents = {}
        self.manifest = {'schemaVersion': 1, 'converter': {'python': '.'.join(map(str, sys.version_info[:2]))}, 'models': []}
        for direction in ('en-zh', 'zh-en'):
            archive = pathlib.Path(self.sources.name) / (direction + '.zip')
            archive.write_bytes(('archive-' + direction).encode())
            model = {'direction': direction, 'archiveUrl': archive.as_uri(),
                     'archiveSha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                     'archiveSize': archive.stat().st_size, 'files': []}
            for name in NAMES:
                data = (direction + '-' + name).encode()
                self.contents[(direction, name)] = data
                model['files'].append({'file': 'resources/translation/' + direction + '/' + name,
                                       'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)})
            self.manifest['models'].append(model)
        self.write_manifest()

    # 只移除当前用例自行创建的根。
    # 入参：无。返回：无。
    def tearDown(self):
        self.temporary.cleanup()
        self.sources.cleanup()

    # 将测试配方保存到真实准备入口使用的位置。
    # 入参：无。返回：无。
    def write_manifest(self):
        (self.root / 'packaging/translation/models.json').write_text(json.dumps(self.manifest), encoding='utf-8')

    # 生成与当前合成固定清单一致的完整包，不调用推理或转换依赖。
    # 入参：path 为当前用例内目录。返回：无。
    def emit(self, path):
        for (direction, name), data in self.contents.items():
            destination = path / direction / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)

    # 改变一个固定文件，模拟新清单，旧包仍须完整保留。
    # 入参：无。返回：无。
    def advance_manifest(self):
        data = b'reviewed-new-version'
        self.contents[('en-zh', 'model.bin')] = data
        self.manifest['models'][0]['files'][1].update(sha256=hashlib.sha256(data).hexdigest(), size=len(data))
        self.write_manifest()

    # 建立当前可用的不可变包及选择指针。
    # 入参：无。返回：旧包目录。
    def existing_pack(self):
        identity = 'a' * 32
        path = self.cache / 'packs' / identity
        self.emit(path)
        (self.cache / 'active.txt').write_text(identity + '\n', encoding='ascii')
        return path

    # 调用真实单资产下载脚本，所有测试 URL 仅为 file 或本机回环。
    # 入参：target/url/data 为目标、来源和期望内容。返回：子进程参数列表。
    def download_command(self, target, url, data):
        return [CMAKE, '-DOPEN_ST_ASSET_PATH=' + target.as_posix(), '-DOPEN_ST_ASSET_URL=' + url,
                '-DOPEN_ST_ASSET_SHA256=' + hashlib.sha256(data).hexdigest(), '-DOPEN_ST_ASSET_SIZE=' + str(len(data)),
                '-P', str(self.root / 'cmake/OpenSTModelCache.cmake')]

    # 独立无语言 CMake fixture 只检查 configure，不触共享构建或模型转换。
    # 入参：无。返回：配置子进程结果。
    def configure(self):
        project = ('cmake_minimum_required(VERSION 3.25)\nproject(ModelCacheFixture NONE)\n'
                   'function(find_package)\nmessage(FATAL_ERROR "unexpected package lookup")\nendfunction()\n'
                   'function(execute_process)\nmessage(FATAL_ERROR "unexpected external command")\nendfunction()\n'
                   'include("${CMAKE_SOURCE_DIR}/cmake/OpenSTTranslationModels.cmake")\n')
        (self.root / 'CMakeLists.txt').write_text(project, encoding='utf-8')
        ninja = pathlib.Path(CMAKE).parents[2] / 'Ninja/ninja.exe'
        return subprocess.run([CMAKE, '-S', str(self.root), '-B', str(self.root / 'build'), '-G', 'Ninja',
                               '-DCMAKE_MAKE_PROGRAM=' + str(ninja)], capture_output=True)

    # 普通 configure 的有效/缺失缓存分支都不探测 Python 或启动任何准备子进程。
    # 入参：无。返回：断言结果。
    def test_configure_only_validates_and_reports_explicit_preparation(self):
        missing = self.configure()
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn(b'convert_models.py', missing.stderr)
        self.assertNotIn(b'unexpected', missing.stderr)
        self.assertFalse((self.root / '.cache/translation-converter').exists())
        self.assertFalse((self.root / '.cache/translation-sources').exists())
        self.emit(self.cache)
        valid = self.configure()
        self.assertEqual(valid.returncode, 0, (valid.stdout + valid.stderr).decode('utf-8', errors='replace'))

    # 现有解释器版本/隔离根不匹配时，任何包探测或 pip 操作之前就停止。
    # 入参：无。返回：断言结果。
    def test_invalid_existing_environment_never_runs_pip(self):
        python = self.root / '.cache/translation-converter/Scripts/python.exe'
        python.parent.mkdir(parents=True)
        python.write_bytes(b'fixture-not-executed')
        with mock.patch.object(PREPARATION.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1)) as run:
            with self.assertRaisesRegex(RuntimeError, 'not an isolated Python'):
                PREPARATION.ensure_environment(self.root, self.manifest)
            self.assertEqual(run.call_count, 1)
            self.assertNotIn('pip', run.call_args.args[0])

    # 旧缓存验证不导入转换依赖，也不准备归档、环境或移动模型。
    # 入参：无。返回：断言结果。
    def test_valid_legacy_cache_does_not_prepare_anything(self):
        self.emit(self.cache)
        with mock.patch.object(PREPARATION, 'prepare_archive', side_effect=AssertionError('unexpected download')):
            with mock.patch.object(PREPARATION, 'ensure_environment', side_effect=AssertionError('unexpected Python setup')):
                self.assertEqual(PREPARATION.prepare(self.root, CMAKE), self.cache)
        self.assertFalse((self.cache / 'active.txt').exists())
        self.assertFalse((self.cache / 'packs').exists())

    # 损坏 active 即使旁边有合法旧缓存也必须显式失败，不能掩盖选择状态损坏。
    # 入参：无。返回：断言结果。
    def test_invalid_active_never_falls_back_to_legacy(self):
        self.emit(self.cache)
        for value in ('../escape', 'a' * 31, 'a' * 33, 'G' * 32, 'b' * 32):
            (self.cache / 'active.txt').write_text(value, encoding='ascii')
            ready, _, _ = PREPARATION.verify_cache(self.root, CMAKE)
            self.assertFalse(ready)

    # 合法格式的 active 也不能借 Windows junction 跳到另一目录。
    # 入参：无。返回：断言结果。
    @unittest.skipUnless(os.name == 'nt', 'Windows junction boundary')
    def test_active_rejects_directory_reparse_points(self):
        target = self.cache / 'original'
        self.emit(target)
        packs = self.cache / 'packs'
        packs.mkdir()
        alias = packs / ('b' * 32)
        quote = lambda value: "'" + str(value).replace("'", "''") + "'"
        command = 'New-Item -ItemType Junction -Path ' + quote(alias) + ' -Target ' + quote(target) + ' | Out-Null'
        subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command], check=True)
        (self.cache / 'active.txt').write_text('b' * 32, encoding='ascii')
        ready, _, _ = PREPARATION.verify_cache(self.root, CMAKE)
        self.assertFalse(ready)
        with self.assertRaises(ValueError):
            PREPARATION.checked_path(self.root, alias)
        configured = self.configure()
        self.assertNotEqual(configured.returncode, 0)

    # 验证全部新文件后只切换指针，旧包及其字节保留供旧构建使用。
    # 入参：无。返回：断言结果。
    def test_publishes_complete_pack_without_overwriting_old_version(self):
        old = self.existing_pack()
        previous = (old / 'en-zh/model.bin').read_bytes()
        self.advance_manifest()
        def convert(root, manifest, python, staging):
            self.emit(staging / 'pack')
        with mock.patch.object(PREPARATION, 'ensure_environment', return_value=pathlib.Path(sys.executable)):
            with mock.patch.object(PREPARATION, 'run_conversion', side_effect=convert):
                selected = PREPARATION.prepare(self.root, CMAKE)
        self.assertNotEqual(selected, old)
        self.assertEqual((old / 'en-zh/model.bin').read_bytes(), previous)
        ready, current, _ = PREPARATION.verify_cache(self.root, CMAKE)
        self.assertTrue(ready)
        self.assertEqual(current, selected)
        self.assertFalse(list(self.cache.glob('.prepare-*')))
        self.assertFalse(list(self.root.glob('.cache/translation-sources/*.part')))

    # 任一方向转换/验证失败都不能切换选择指针或破坏旧版本。
    # 入参：无。返回：断言结果。
    def test_bad_candidate_preserves_old_active_and_contents(self):
        old = self.existing_pack()
        previous = (old / 'en-zh/model.bin').read_bytes()
        active = (self.cache / 'active.txt').read_bytes()
        self.advance_manifest()
        def convert(root, manifest, python, staging):
            self.emit(staging / 'pack')
            (staging / 'pack/zh-en/model.bin').write_bytes(b'broken')
        with mock.patch.object(PREPARATION, 'ensure_environment', return_value=pathlib.Path(sys.executable)):
            with mock.patch.object(PREPARATION, 'run_conversion', side_effect=convert):
                with self.assertRaises(ValueError):
                    PREPARATION.prepare(self.root, CMAKE)
        self.assertEqual((self.cache / 'active.txt').read_bytes(), active)
        self.assertEqual((old / 'en-zh/model.bin').read_bytes(), previous)
        self.assertEqual(len(list((self.cache / 'packs').iterdir())), 1)
        self.assertFalse(list(self.cache.glob('.prepare-*')))

    # 指针文件替换失败时仅多留下一个完整未选中的包，旧选择仍有效且不自动清理版本。
    # 入参：无。返回：断言结果。
    def test_pointer_failure_keeps_previous_package_selected(self):
        old = self.existing_pack()
        active = (self.cache / 'active.txt').read_bytes()
        staging = self.cache / '.prepare-fixture'
        self.emit(staging / 'pack')
        with mock.patch.object(PREPARATION.os, 'replace', side_effect=PermissionError('injected pointer failure')):
            with self.assertRaises(PermissionError):
                PREPARATION.publish_candidate(self.root, staging / 'pack')
        self.assertEqual((self.cache / 'active.txt').read_bytes(), active)
        self.assertTrue(old.is_dir())
        self.assertFalse(list(self.cache.glob('.active-*.part')))

    # Windows 拒绝目录重命名时，旧 active、旧包和完整候选都保持原状，不宣称发布成功。
    # 入参：无。返回：断言结果。
    def test_directory_rename_denial_preserves_previous_selection(self):
        old = self.existing_pack()
        active = (self.cache / 'active.txt').read_bytes()
        previous = (old / 'en-zh/model.bin').read_bytes()
        staging = self.cache / '.prepare-fixture'
        self.emit(staging / 'pack')
        with mock.patch.object(PREPARATION.os, 'rename', side_effect=PermissionError('injected directory denial')):
            with self.assertRaises(PermissionError):
                PREPARATION.publish_candidate(self.root, staging / 'pack')
        self.assertEqual((self.cache / 'active.txt').read_bytes(), active)
        self.assertEqual((old / 'en-zh/model.bin').read_bytes(), previous)
        self.assertTrue((staging / 'pack/zh-en/model.bin').is_file())
        self.assertEqual(len(list((self.cache / 'packs').iterdir())), 1)
        self.assertFalse(list(self.cache.glob('.active-*.part')))

    # 第二个独立进程必须等单写者释放，然后重检已发布包并跳过全部准备动作。
    # 入参：无。返回：断言结果。
    def test_second_process_waits_and_rechecks_after_lock(self):
        script = ('import importlib.util,pathlib,sys; '
                  's=importlib.util.spec_from_file_location("prep",sys.argv[1]); '
                  'm=importlib.util.module_from_spec(s); s.loader.exec_module(m); '
                  'm.ensure_environment=lambda *a: (_ for _ in ()).throw(AssertionError("unexpected setup")); '
                  'print("waiting",flush=True); m.prepare(pathlib.Path(sys.argv[2]),sys.argv[3])')
        process = None
        try:
            with PREPARATION.preparation_lock(self.root):
                process = subprocess.Popen([sys.executable, '-I', '-B', '-X', 'utf8', '-c', script, str(SPEC.origin), str(self.root), CMAKE],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8',
                                           close_fds=True)
                self.assertEqual(process.stdout.readline().strip(), 'waiting')
                time.sleep(0.15)
                self.assertIsNone(process.poll())
                staging = self.cache / '.prepare-fixture'
                self.emit(staging / 'pack')
                PREPARATION.publish_candidate(self.root, staging / 'pack')
            output, error = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 0, output + error)
            self.assertIn('Verified existing', output)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.communicate()

    # 下载失败只清理本次随机 .part，原来的目标字节保留。
    # 入参：无。返回：断言结果。
    def test_bad_download_preserves_old_asset(self):
        target = self.root / 'asset.bin'
        target.write_bytes(b'old-valid-for-prior-manifest')
        incoming = self.root / 'source.bin'
        incoming.write_bytes(b'wrong')
        result = subprocess.run(self.download_command(target, incoming.as_uri(), b'expected'), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_bytes(), b'old-valid-for-prior-manifest')
        self.assertFalse(list(self.root.glob('asset.bin.*.part')))

    # 两个 CMake 进程同时缺缓存只下载一次，第二个锁后重检命中第一份完整文件。
    # 入参：无。返回：断言结果。
    def test_concurrent_cmake_downloads_have_one_writer(self):
        started = threading.Event()
        release = threading.Event()
        requests = []
        data = b'fixed-local-asset'
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                started.set()
                release.wait(5)
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            def log_message(self, *args):
                pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        processes = []
        try:
            target = self.root / 'asset.bin'
            command = self.download_command(target, 'http://127.0.0.1:' + str(server.server_port) + '/asset', data)
            environment = os.environ.copy()
            environment.update(NO_PROXY='127.0.0.1', no_proxy='127.0.0.1')
            processes.append(subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment))
            if not started.wait(5):
                output, error = processes[0].communicate(timeout=5)
                self.fail((output + error).decode('utf-8', errors='replace'))
            processes.append(subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment))
            time.sleep(0.2)
            self.assertEqual(len(requests), 1)
            self.assertEqual(len(list(self.root.glob('asset.bin.*.part'))), 1)
            release.set()
            for process in processes:
                output, error = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, (output + error).decode('utf-8', errors='replace'))
            self.assertEqual(len(requests), 1)
            self.assertEqual(target.read_bytes(), data)
            self.assertFalse(list(self.root.glob('asset.bin.*.part')))
        finally:
            release.set()
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == '__main__':
    unittest.main()
