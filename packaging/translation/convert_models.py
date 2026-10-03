"""显式准备固定 OPUS 模型；普通 configure 仅验证缓存，不安装 Python 依赖或转换。"""
import argparse
import contextlib
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
import venv
import zipfile


# 校验固定资产的完整字节，避免一次把权重全部复制进校验器内存。
# 入参：path/digest/size 为路径、固定摘要和长度。返回：完整匹配状态。
def matches(path, digest, size):
    if not path.is_file() or path.is_symlink() or path.stat().st_size != size:
        return False
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(chunk)
    return checksum.hexdigest() == digest


# 限制准备文件位于仓库内部，拒绝任一已有路径组件的重解析点。
# 入参：root 为已解析仓库根，path 为缓存子路径。返回：原路径；逃逸时抛异常。
def checked_path(root, path):
    if not path.is_relative_to(root):
        raise ValueError('Model preparation path escapes the repository')
    for current in (path, *path.parents):
        if current == root:
            break
        try:
            info = current.lstat()
        except FileNotFoundError:
            continue
        if (stat.S_ISLNK(info.st_mode)
                or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
            raise ValueError('Model preparation refuses reparse points: ' + str(current))
    if not path.resolve().is_relative_to(root):
        raise ValueError('Model preparation path resolves outside the repository')
    return path


# 显式准备的唯一跨进程写锁；锁文件保持存在，进程退出由系统回收字节锁。
# 入参：root 为仓库根，timeout 为最长等待秒数。返回：持锁作用域。
@contextlib.contextmanager
def preparation_lock(root, timeout=600):
    cache = checked_path(root, root / '.cache/translation-models')
    cache.mkdir(parents=True, exist_ok=True)
    lock_path = checked_path(root, cache / '.prepare.lock')
    with lock_path.open('a+b') as stream:
        if stream.seek(0, os.SEEK_END) == 0:
            stream.write(b'\0')
            stream.flush()
        deadline = time.monotonic() + timeout
        while True:
            try:
                stream.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Another model preparation process still holds the cache lock')
                time.sleep(0.1)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == 'nt':
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


# 显式准备复用构建机已有 CMake，不下载或安装工具。
# 入参：explicit 为可选工具路径。返回：已有 cmake.exe 路径。
def find_cmake(explicit):
    if explicit:
        candidate = pathlib.Path(explicit).resolve()
    else:
        found = shutil.which('cmake')
        if not found and os.name == 'nt':
            vswhere = pathlib.Path(os.environ.get('ProgramFiles(x86)', '')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
            if vswhere.is_file():
                found = subprocess.check_output([
                    str(vswhere), '-latest', '-products', '*', '-find',
                    'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'], text=True).strip()
        if not found:
            raise RuntimeError('CMake is required for explicit preparation; specify --cmake <cmake.exe>')
        candidate = pathlib.Path(found)
    if not candidate.is_file():
        raise ValueError('CMake executable does not exist')
    return str(candidate)


# 只调用 CMake 唯一选择/校验入口；显式候选不发布、不覆盖 active。
# 入参：root/cmake 为项目和工具，candidate 为可选整套目录。返回：有效状态、固定目录、原因。
def verify_cache(root, cmake, candidate=None):
    cache = checked_path(root, root / '.cache/translation-models')
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.check-', dir=cache) as temporary:
        report = pathlib.Path(temporary) / 'result.txt'
        command = [cmake, '-DOPEN_ST_TRANSLATION_ROOT=' + root.as_posix(),
                   '-DOPEN_ST_TRANSLATION_REPORT=' + report.as_posix()]
        if candidate is not None:
            command.append('-DOPEN_ST_TRANSLATION_CANDIDATE=' + candidate.as_posix())
        subprocess.run(command + ['-P', str(root / 'cmake/OpenSTTranslationModels.cmake')], check=True)
        lines = report.read_text(encoding='utf-8').splitlines()
        return lines[0] == 'TRUE', pathlib.Path(lines[1]) if lines[1] else None, '\n'.join(lines[2:])


# 原始归档走 OCR 共用的下载器、资产锁、唯一临时文件及固定校验。
# 入参：root/cmake 为项目和工具，model 为固定清单项。返回：已校验归档路径。
def prepare_archive(root, cmake, model):
    direction = model['direction']
    if direction not in ('en-zh', 'zh-en'):
        raise ValueError('Unknown fixed model direction')
    archive = checked_path(root, root / '.cache/translation-sources' / (direction + '-original.zip'))
    subprocess.run([cmake, '-DOPEN_ST_ASSET_PATH=' + archive.as_posix(),
                    '-DOPEN_ST_ASSET_URL=' + model['archiveUrl'],
                    '-DOPEN_ST_ASSET_SHA256=' + model['archiveSha256'],
                    '-DOPEN_ST_ASSET_SIZE=' + str(model['archiveSize']),
                    '-P', str(root / 'cmake/OpenSTModelCache.cmake')], check=True)
    return archive


# 从唯一依赖清单构造版本校验，不在代码中维护第二份转换依赖版本表。
# 入参：requirements 为固定带哈希 requirements 文件。返回：包名到版本的字典。
def dependency_versions(requirements):
    return dict(re.findall(r'^([A-Za-z0-9_-]+)==([^\s\\]+)', requirements.read_text(encoding='utf-8'), re.MULTILINE))


# 只有显式准备且缓存无效时才创建工作区环境和安装固定二进制 wheel。
# 入参：root 为仓库，manifest 为固定转换配方。返回：隔离环境解释器。
def ensure_environment(root, manifest):
    expected_python = manifest['converter']['python']
    if '.'.join(map(str, sys.version_info[:2])) != expected_python:
        raise RuntimeError('Explicit model preparation requires Python ' + expected_python + ' from models.json')
    if os.name != 'nt' or sys.maxsize <= 2**32:
        raise RuntimeError('The reviewed converter recipe requires Windows x64 Python')
    environment = checked_path(root, root / '.cache/translation-converter')
    python = environment / 'Scripts/python.exe'
    if not python.is_file():
        venv.EnvBuilder(with_pip=True).create(environment)
    checked_path(root, python)
    # 安装前先验证现有解释器的版本与真正隔离根，防止错误环境把依赖写入宿主。
    expected_version = tuple(int(part) for part in expected_python.split('.'))
    runtime_probe = ('import pathlib,sys; assert sys.version_info[:2]==' + repr(expected_version)
                     + '; assert sys.prefix != sys.base_prefix; assert pathlib.Path(sys.prefix).resolve()=='
                     + 'pathlib.Path(' + repr(str(environment)) + ').resolve()')
    runtime = subprocess.run([str(python), '-I', '-c', runtime_probe],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if runtime.returncode != 0:
        raise RuntimeError('Existing .cache/translation-converter is not an isolated Python '
                           + expected_python + ' environment; recreate it explicitly before preparing models')
    requirements = root / 'packaging/translation/converter-requirements.txt'
    versions = dependency_versions(requirements)
    for name, field in (('ctranslate2', 'ctranslate2'), ('numpy', 'numpy'), ('PyYAML', 'pyyaml')):
        if versions.get(name) != manifest['converter'][field]:
            raise ValueError('Requirements differ from the reviewed conversion recipe')
    probe = ('import importlib.metadata as m,sys; expected=' + repr(versions)
             + '; assert all(m.version(k)==v for k,v in expected.items())'
             + '; assert sys.version_info[:2]==' + repr(expected_version))
    command = [str(python), '-I', '-c', probe]
    ready = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not ready:
        subprocess.run([str(python), '-I', '-m', 'pip', '--isolated', 'install', '--disable-pip-version-check',
                        '--only-binary=:all:', '--require-hashes', '--no-deps', '-r', str(requirements)], check=True)
        subprocess.run(command, check=True)
    return python


# 隔离转换 worker 仅写父进程新建的唯一 staging，绝不发布或覆盖可用缓存。
# 入参：root/manifest 为仓库配方，staging 为受限临时目录。返回：完整候选包目录。
def convert_candidate(root, manifest, staging):
    import ctranslate2
    import numpy
    import yaml
    if (ctranslate2.__version__ != manifest['converter']['ctranslate2']
            or numpy.__version__ != manifest['converter']['numpy']
            or yaml.__version__ != manifest['converter']['pyyaml']):
        raise ValueError('Converter tool versions differ from the reviewed recipe')
    checked_path(root, staging)
    if staging.parent != root / '.cache/translation-models' or not staging.name.startswith('.prepare-'):
        raise ValueError('Converter worker requires a dedicated staging directory')
    pack = staging / 'pack'
    pack.mkdir()
    for model in manifest['models']:
        direction = model['direction']
        if direction not in ('en-zh', 'zh-en'):
            raise ValueError('Unknown fixed model direction')
        archive = root / '.cache/translation-sources' / (direction + '-original.zip')
        if not matches(archive, model['archiveSha256'], model['archiveSize']):
            raise ValueError('Original model archive integrity mismatch')
        extracted = staging / direction
        extracted.mkdir()
        with zipfile.ZipFile(archive) as bundle:
            for name in ('decoder.yml', 'opus.spm32k-spm32k.vocab.yml',
                         'opus.spm32k-spm32k.transformer.model1.npz.best-perplexity.npz',
                         'source.spm', 'target.spm', 'LICENSE', 'README.md'):
                (extracted / name).write_bytes(bundle.read(name))
        converted = pack / direction
        ctranslate2.converters.OpusMTConverter(str(extracted)).convert(
            str(converted), quantization=manifest['converter']['quantization'])
        for name in ('source.spm', 'target.spm', 'LICENSE', 'README.md'):
            shutil.copyfile(extracted / name, converted / name)
    return pack


# 父进程保持准备锁，worker 不拥有发布权限，只负责受控环境内的同步转换。
# 入参：root/manifest 为配方，python 为受控解释器，staging 为唯一工作目录。返回：无。
def run_conversion(root, manifest, python, staging):
    subprocess.run([str(python), '-I', str(root / 'packaging/translation/convert_models.py'),
                    '--root', str(root), '--worker', str(staging)], check=True)


# 完整候选发布到全新不可变目录，最后只替换小指针文件；不伪称 Windows 目录替换原子。
# 入参：root 为仓库，candidate 为已验证整套。返回：新的固定目录；失败保留旧 active/旧包。
def publish_candidate(root, candidate):
    cache = root / '.cache/translation-models'
    packs = checked_path(root, cache / 'packs')
    packs.mkdir(exist_ok=True)
    identity = uuid.uuid4().hex
    destination = checked_path(root, packs / identity)
    pointer = checked_path(root, cache / 'active.txt')
    temporary = checked_path(root, cache / ('.active-' + uuid.uuid4().hex + '.part'))
    os.rename(candidate, destination)
    try:
        with temporary.open('x', encoding='ascii', newline='\n') as stream:
            stream.write(identity + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, pointer)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


# 显式入口持锁后重检；旧缓存有效时不下载、不建环境、不转换、不搬运大模型。
# 入参：root/cmake 为仓库与既有工具。返回：已验证固定目录。
def prepare(root, cmake):
    manifest = json.loads((root / 'packaging/translation/models.json').read_text(encoding='utf-8'))
    with preparation_lock(root):
        ready, selected, _ = verify_cache(root, cmake)
        if ready:
            print('Verified existing translation model cache: ' + str(selected))
            return selected
        for model in manifest['models']:
            prepare_archive(root, cmake, model)
        python = ensure_environment(root, manifest)
        cache = root / '.cache/translation-models'
        with tempfile.TemporaryDirectory(prefix='.prepare-', dir=cache) as temporary:
            staging = pathlib.Path(temporary)
            run_conversion(root, manifest, python, staging)
            candidate = staging / 'pack'
            ready, _, reason = verify_cache(root, cmake, candidate)
            if not ready:
                raise ValueError('Converted model package failed verification: ' + reason)
            selected = publish_candidate(root, candidate)
            print('Published verified translation model cache: ' + str(selected))
            return selected


# 命令行只提供显式准备；worker 是父进程保持锁期间的内部转换阶段。
# 入参：--root 项目根，--cmake 可选既有工具。返回：成功退出；错误保留旧模型并失败。
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=pathlib.Path)
    parser.add_argument('--cmake')
    parser.add_argument('--worker', type=pathlib.Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    root = args.root.resolve()
    if args.worker is not None:
        manifest = json.loads((root / 'packaging/translation/models.json').read_text(encoding='utf-8'))
        convert_candidate(root, manifest, args.worker.resolve())
    else:
        prepare(root, find_cmake(args.cmake))


if __name__ == '__main__':
    main()
