# Parabix
> Parabix technology is a high-performance programming framework for streaming text processing applications, leveraging both SIMD and multicore parallel processing features.

### Documentation
For more information about Parabix, please check our [wiki](https://cs-git-research.cs.sfu.ca/cameron/parabix-devel/-/wikis/home).

### Tools
Parabix includes a set of built-in [tools](https://cs-git-research.cs.sfu.ca/cameron/parabix-devel/-/tree/master/tools), such as [`icgrep`](README-icgrep.md), which is a very fast regular expression search program.

### Requirements

To build Parabix, you need a development environment that meets a few requirements.

-  Standard C++ development tools including git, C++, etc.
-  A modern C++ compiler supporting at least C++17.
-  The [`cmake`](https://cmake.org/download/) build system version 3.12 or better (3.14 or better to install).
-  [`Boost`](https://www.boost.org/users/download/) libraries version `1.61` or better (Ubuntu `libboost-all-dev`).
-  [`LLVM`](https://releases.llvm.org/download.html) version `17` or later (built in Release mode).
-  [`ICU`](https://icu.unicode.org/) (Ubuntu `libicu-dev`, Homebrew `icu4c`): required by the `tokenizer` tool, and used by `tconv` if found.
-  Python 3, to run the test suite.

Linux, the BSDs and macOS are supported.

### Building for development

- Clone parabix-devel from the repository:
  `git clone https://cs-git-research.cs.sfu.ca/cameron/parabix-devel.git`
- Create a build directory and the makefiles:
  ```
  cd parabix-devel
  mkdir build
  cd build
  cmake -DCMAKE_BUILD_TYPE=Release ..
  ```
  If you have built or installed LLVM somewhere CMake does not search, give its location:
  `cmake -DCMAKE_PREFIX_PATH=/path/to/llvm -DCMAKE_BUILD_TYPE=Release ..` (or `-DLLVM_PATH=/path/to/llvm`).
- `make -j8` builds everything; `make -j8 icgrep` builds just one tool (and the libraries it needs).
- `make check` builds everything and runs the test suite.  `-DPARABIX_BUILD_TESTS=OFF` leaves
  the kernel and unit test programs out of the build.

The tools are then in `build/bin`.  A development build makes a shared library of each
framework module (in `build/lib`), which the tools in `build/bin` use where they are: it is
not meant to be installed (see below).

### Object cache

Parabix tools compile their kernels at run time.  The compiled kernels are cached, so that
later runs start quickly: in `~/.parabix/objcache` of the user running a tool (created when
first needed), or in `$PARABIX_OBJECT_CACHE_DIR` if that is set.  `-object-cache-dir=<dir>`
uses another directory for one run, `-enable-object-cache=0` turns the cache off, and
`-trace-object-cache` reports what is read and written.  A background process
(`cachejanitord`) removes the entries not used for a week.

### Installing

Installations are made from a build configured with `-DARTIFACT_MODE=Single`, which puts
the whole framework in one shared library, `libparabix`:

```
mkdir build-install
cd build-install
cmake -DCMAKE_BUILD_TYPE=Release -DARTIFACT_MODE=Single -DPARABIX_BUILD_TESTS=OFF ..
make -j8 install-tools
```

By default this installs the tools for your own use, under `~/.local` (make sure that
`~/.local/bin` is on your `PATH`).  The configuration options:

| Option | Values |
|---|---|
| `PARABIX_INSTALL_SCOPE` | `User` (default prefix `~/.local`), or `System` for all users (default prefix `/usr/local`) |
| `CMAKE_INSTALL_PREFIX` | Another installation prefix |
| `PARABIX_INSTALL_TOOLS` | The tools to install: `default`, `dev`, `all` (both), and tool names, e.g. `"default;ucount"` |

The tool sets:

- `default`: `icgrep`, `wc`, `base64`, `csvcut`, `csvgrep`, `csv2json`, `tconv`, `ldml_trules`,
  `hexlines`, the transcoders `gb18030`, `u32u16`, `u32u8`, `u8u16`, `x8u16`, and the Unicode
  normalizers `nfc`, `nfd`, `nfkd`.
- `dev`: developer tools and demonstrations (`ucount`, `tokenizer`, `xch`, `editd`,
  `idisa_exerciser`, `idisa_test`, `hashdemo`, `matchprior`, `ufilter`, the audio tools,
  `ztf-runs`, `ztf-hash`).
- `json`, `xml` and `ztf1` read Pablo source files at run time.  They are in no set, and can be
  installed (by name) only for your own use.

The make targets:

| Target | Effect |
|---|---|
| `make install-tools` | Builds and installs the tools of `PARABIX_INSTALL_TOOLS` |
| `make install-<tool>` | Builds and installs one tool, of any set |
| `make install` | Builds everything, and installs the tools of `PARABIX_INSTALL_TOOLS` |
| `make installcheck` | Runs the test suite on the installed tools (the tests of tools that are installed) |
| `make uninstall-<tool>` | Uninstalls one tool; the library goes with the last tool |
| `make uninstall` | Uninstalls everything |

`DESTDIR=<dir>` stages any installation in another directory.  Uninstalling does not need
the build directory: `cmake [-DTOOL=<tool>] -P <prefix>/share/parabix/uninstall.cmake`.

The installed files:

| Path (below the prefix) | Contents |
|---|---|
| `bin/icgrep`, `bin/px-<tool>` | The tools; all but `icgrep` are prefixed, so as not to hide system commands such as `wc` and `base64` |
| `lib/libparabix.so.0.9` (`libparabix.0.9.dylib` on macOS) | The Parabix library |
| `libexec/parabix/cachejanitord` | The object cache cleanup process |
| `libexec/parabix/bin/` | Unprefixed links to `wc`, `base64`, `csvgrep`, `csvcut`, `csv2json`, `tconv` and `hexlines`, for those who put this directory on their `PATH` |
| `share/parabix/` | Pablo sources, the records of the installed files, and the uninstall script |
| `share/doc/parabix/` | READMEs and licenses |

An installation can be moved: the tools find the library and their other files relative to
their own location.  Boost is linked into `libparabix`, so the tools do not depend on the
Boost libraries of the machine that built them.  The static Boost libraries need some
libraries of their own when linked: the development packages of bzip2, xz (lzma) and zstd
(Debian and Ubuntu `libbz2-dev liblzma-dev libzstd-dev`, Fedora `bzip2-devel xz-devel
libzstd-devel`; Homebrew's Boost also needs `icu4c`).  Without them, configure with
`-DPARABIX_STATIC_BOOST=OFF` to link Boost dynamically instead (the installed tools then use
the Boost libraries of the machine).  Static libraries can be part of `libparabix` only if they
were compiled to be position-independent (`-fPIC`), which the x86-64 Boost packages of Debian
and Ubuntu are not: configuring checks this, and links Boost dynamically when they are not.

### Installing for all users

```
cmake -DCMAKE_BUILD_TYPE=Release -DARTIFACT_MODE=Single -DPARABIX_BUILD_TESTS=OFF \
      -DPARABIX_INSTALL_SCOPE=System ..
make -j8
sudo make install-tools
```

A system installation also has a system-wide object cache: `/var/cache/parabix`
(`/Library/Caches/parabix` on macOS), or `-DPARABIX_SYSTEM_OBJECT_CACHE_DIR=<dir>`
(`none` for no system-wide cache).  Installing fills it by running each installed tool once
(`share/parabix/prewarm.sh`), with the kernels that do not depend on what a run asks for.
The tools read it after the user's own cache and never write to it; what neither cache has
is compiled into the user's cache.  The system-wide cache is used only if it belongs to root
(or to the user running the tool) and no one else can write to it.  Uninstalling removes it.

Kernels are compiled for the processor of the machine compiling them, so the system-wide
cache is not filled by staged installations (`DESTDIR`, or packages); run
`sudo sh <prefix>/share/parabix/prewarm.sh` on the machine where the tools are installed.

### Packages

CPack makes binary packages for installing for all users, from a build configured as above
(with `-DPARABIX_INSTALL_SCOPE=System`):

```
make -j8
cpack                # TGZ, and DEB and RPM where dpkg-deb and rpmbuild are available
cpack -G DEB         # one kind
```

The DEB and RPM packages install at the configured prefix, fill the system-wide object cache
when installed, and empty it when removed.  Set the maintainer named in them with
`-DPARABIX_PACKAGE_MAINTAINER="Name <address>"`.  A TGZ package is unpacked at the prefix it
was configured for, after which the cache is filled by hand:

```
sudo tar xzf parabix-0.9.0-<system>.tar.gz -C <prefix> --strip-components=1
sudo sh <prefix>/share/parabix/prewarm.sh
```

### Papers

[Bitwise Data Parallelism with LLVM: The ICgrep Case Study](https://link.springer.com/chapter/10.1007%2F978-3-319-27122-4_26). Robert D. Cameron, Nigel Medforth, Dan Lin, Dale Denis, William N. Sumner. ICA3PP (2) 2015: 373-387

[Bitwise data parallelism in regular expression matching](https://dl.acm.org/doi/10.1145/2628071.2628079). Robert D. Cameron, Thomas C. Shermer, Arrvindh Shriraman, Kenneth S. Herdy, Dan Lin, Benjamin R. Hull, Meng Lin. PACT 2014: 139-150

[icXML: Accelerating a Commercial XML Parser Using SIMD and Multicore Technologies](http://www.balisage.net/Proceedings/vol10/html/Cameron01/BalisageVol10-Cameron01.html). Presented at Balisage: The Markup Conference 2013, Montréal, Canada, August 6 - 9, 2013.

[Parabix: Boosting the Efficiency of Text Processing on Commodity Processors](http://www.cs.sfu.ca/~ashriram/publications/2012_HPCA_Parabix.pdf), 18th International Symposium on High Performance Computer Architecture in New Orleans, Louisiana, February 2012

[Parallel Scanning with Bitstream Addition: An XML Case Study](http://parabix.costar.sfu.ca/export/901/docs/EuroPar2011/europar-cameron.pdf), Euro-Par 2011 Conference, Bordeaux, France, September 2011.

[Parallel Bit Stream Technology as a Foundation for XML Parsing Performance](http://www.balisage.net/Proceedings/vol4/html/Cameron01/BalisageVol4-Cameron01.html), 2009 International Symposium on Processing XML Efficiently: Overcoming Limits on Space, Time, or Bandwidth, Montreal, Canada, August 2009.

[Architectural Support for SIMD Text Processing with Parallel Bit Streams: The Inductive Doubling Principle](http://parabix.costar.sfu.ca/export/901/docs/ASPLOS09/asplos094-cameron.pdf), Fourteenth International Conference on Architectural Support for Programming Languages and Operating Systems (ASPLOS '09), Washington, DC, March 2009.

### License
LLVM files are governed by the LLVM Release License in [LLVM-LICENSE.txt](LLVM-LICENSE.txt). `Parabix` is governed by Open Software License `3.0` in [OSL3.0.txt](OSL3.0.txt).
