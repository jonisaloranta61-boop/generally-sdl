#!/usr/bin/env python3
"""Muokkaa Winlator-lahdekoodia: GeneRally-paketti. Kayttö: patch.py <winlator-app-hakemisto>"""
import sys, os
root = sys.argv[1]
def edit(path, old, new):
    p = os.path.join(root, path)
    s = open(p, encoding='utf-8').read()
    assert s.count(old) == 1, (path, old, s.count(old))
    open(p, 'w', encoding='utf-8').write(s.replace(old, new))

J = 'app/src/main/java/com/winlator/'
# 1) paaikkuna: kaynnista GeneRally-valinta rootfs-asennuksen jalkeen
edit(J + 'MainActivity.java',
     'if (!requestAppPermissions()) RootFSInstaller.installIfNeeded(this);',
     'if (!requestAppPermissions()) { RootFSInstaller.installIfNeeded(this); AutoGame.begin(this); }')
edit(J + 'MainActivity.java',
     '                RootFSInstaller.installIfNeeded(this);\n            }\n            else finish();',
     '                RootFSInstaller.installIfNeeded(this);\n                AutoGame.begin(this);\n            }\n            else finish();')
# 2) rootfs-asennuksen jalkeen
edit(J + 'xenvironment/RootFSInstaller.java',
     '            dialog.closeOnUiThread();\n        });\n    }\n\n    public static void installIfNeeded',
     '            dialog.closeOnUiThread();\n            if (success) activity.runOnUiThread(() -> com.winlator.AutoGame.begin(activity));\n        });\n    }\n\n    public static void installIfNeeded')
# 3) pelin sulkeminen sulkee sovelluksen (ei palaa valikkoon)
edit(J + 'XServerDisplayActivity.java',
     '    private void exit() {\n        winHandler.stop();\n        if (environment != null) environment.stopEnvironmentComponents();\n',
     '    private void exit() {\n        winHandler.stop();\n        if (environment != null) environment.stopEnvironmentComponents();\n'
     '        if (getIntent().getBooleanExtra("auto_game", false)) {\n            ForegroundService.stopSession(this);\n            finishAndRemoveTask();\n            Runtime.getRuntime().exit(0);\n            return;\n        }\n')
print("patch OK")
