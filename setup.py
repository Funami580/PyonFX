import os
import re
import setuptools
from setuptools.command.develop import develop
from setuptools.command.install import install
import subprocess
from pathlib import Path

here = os.path.abspath(os.path.dirname(__file__))


def read(*parts):
    with open(os.path.join(here, *parts), "r") as fp:
        return fp.read()


def find_version(*file_paths):
    version_file = read(*file_paths)
    version_match = re.search(r"^__version__ = [\'\"](.+)[\'\"]", version_file, re.M)
    if version_match:
        return version_match.group(1)
    raise RuntimeError("Unable to find version string.")


def make_libass():
    current_path = os.path.realpath(Path(__file__).parent)
    libass_path = os.path.join(current_path, "pyonfx/libass/")
    os.chdir(libass_path)
    return_code = subprocess.call("./autogen.sh && ./configure && make", shell=True)

    if return_code != 0:
        raise Exception("Building libass failed")

    os.chdir(current_path)


class PreDevelopCommand(develop):
    def run(self):
        make_libass()
        develop.run(self)


class PreInstallCommand(install):
    def run(self):
        make_libass()
        install.run(self)


setuptools.setup(
    name="pyonfx",
    url="https://github.com/CoffeeStraw/PyonFX",
    author="Antonio Strippoli",
    author_email="clarantonio98@gmail.com",
    version=find_version("pyonfx", "__init__.py"),
    license="GNU LGPL 3.0 or later",
    description="An easy way to do KFX and complex typesetting based on subtitle format ASS (Advanced Substation Alpha).",
    long_description=open("README.md", encoding="utf-8").read(),
    packages=["pyonfx", "pyonfx.python_ass", "pyonfx.python_ass.ass"],
    include_package_data=True,
    install_requires="pyquaternion",
    extras_require={
        "dev": ["pytest", "pytest-check", "sphinx_rtd_theme", "sphinxcontrib-napoleon"]
    },
    classifiers=[
        "Development Status :: 2 - Pre-Alpha",
        "Programming Language :: Python",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
    ],
    cmdclass={
        'develop': PreDevelopCommand,
        'install': PreInstallCommand,
    },
)
