# Qt 6.10.3 Runtime Notice

The Windows `ccs-trans-gui.exe` distribution dynamically links unmodified Qt
6.10.3 libraries from the official `win64_mingw` binary packages. The deployed
Qt Base and Qt Declarative components are used under the GNU Lesser General
Public License version 3. The corresponding license text and exact binary SPDX
documents are included in `THIRD_PARTY_LICENSES` beside the application.

Users may replace the deployed Qt shared libraries with compatible modified
versions. The project does not use a static Qt build, does not apply technical
measures that prevent relinking, and does not claim Qt as part of ccs-trans.

Qt source code for this exact release is available from:

https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/

Qt and the Qt logo are trademarks of The Qt Company Ltd. This notice is not an
endorsement by The Qt Company.
