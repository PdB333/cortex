CORTEX v0.6.0 - INSTALLATION ET DEMARRAGE RAPIDE
================================================

Cortex est fourni en deux archives distinctes : Windows x86 et Windows x64.
Choisissez TOUJOURS l'archive correspondant a l'architecture du jeu ou du
programme cible :

- jeu 32 bits  -> archive x86
- jeu 64 bits  -> archive x64

Un injecteur x86 ne peut pas injecter un processus x64, et inversement.

CONTENU PRINCIPAL DU ZIP
------------------------

cortex_core.dll
    Agent Cortex charge dans le processus cible.

cortex.asi
    Copie strictement identique de cortex_core.dll avec l'extension .asi.
    Utilisez ce fichier uniquement avec un ASI loader compatible.

injector.exe
    Injecteur autonome compatible avec l'utilisation historique de Cortex.

cortex_host.exe
    Outil principal unifie. Il fournit les commandes serve, inject, diagnose,
    analyze, symbolize et mcp.

cortex_test_target_x86.exe / cortex_test_target_x64.exe
    Petit programme de demonstration permettant de tester Cortex sans jeu.

README.md
    Documentation complete.

README_INSTALL.txt
    Ce tutoriel.

CHANGELOG.md, LICENSE, docs, sdk, agent
    Historique, licence, documentation technique, SDK et documentation agent.


METHODE 1 - INJECTEUR AUTONOME
------------------------------

1. Decompressez toute l'archive dans un dossier, par exemple :

       C:\Cortex\

2. Lancez le jeu ou le programme cible.

3. Ouvrez PowerShell ou l'invite de commandes dans le dossier Cortex.

4. Injectez par nom de processus :

       .\injector.exe game.exe

   ou par PID :

       .\injector.exe 1234

5. Par defaut, injector.exe charge cortex_core.dll situe dans le meme dossier.
   Vous pouvez aussi donner un chemin explicite :

       .\injector.exe game.exe .\cortex_core.dll

6. En cas d'erreur OpenProcess, relancez le terminal en administrateur seulement
   si les droits Windows du processus cible l'exigent.


METHODE 2 - CORTEX_HOST.EXE
---------------------------

L'outil unifie peut egalement injecter Cortex :

       .\cortex_host.exe inject game.exe

ou :

       .\cortex_host.exe inject 1234 .\cortex_core.dll

Commandes principales :

       cortex_host.exe serve ...
       cortex_host.exe inject ...
       cortex_host.exe probe --pid ...
       cortex_host.exe diagnose ...
       cortex_host.exe analyze ...
       cortex_host.exe symbolize ...
       cortex_host.exe mcp ...

Affichez l'aide avec :

       .\cortex_host.exe help


MCP NATIF POUR CLIENTS IA
-------------------------

v0.6.0 utilise par defaut un transport MCP stdio -> Named Pipe Windows local et
authentifie. Le bridge n'a plus besoin de repasser par HTTP pour executer les
outils MCP.

Pour injecter Cortex et demarrer MCP en une seule commande :

       .\cortex_host.exe mcp --process game.exe

Ou, si Cortex est deja injecte :

       .\cortex_host.exe mcp --token-file .\cortex.token

Le profil par defaut expose les 30 outils semantiques. Pour exposer aussi les
primitives bas niveau :

       .\cortex_host.exe mcp --token-file .\cortex.token --tools all

Le transport HTTP historique reste disponible explicitement pour compatibilite
et diagnostic :

       .\cortex_host.exe mcp --transport http --token-file .\cortex.token


METHODE 3 - ASI LOADER
----------------------

Cette methode demande un ASI loader deja installe et compatible avec le jeu.

1. Copiez cortex.asi dans le dossier attendu par l'ASI loader, souvent :

       <dossier_du_jeu>\scripts\

   ou directement dans le dossier du jeu selon le loader utilise.

2. Lancez le jeu normalement.

3. Ne chargez pas simultanement cortex.asi et cortex_core.dll : cela tenterait de
   charger Cortex deux fois dans le meme processus.

cortex.asi et cortex_core.dll contiennent exactement le meme binaire. Seule
l'extension change pour faciliter l'utilisation avec un ASI loader.


TEST RAPIDE SANS JEU
--------------------

1. Lancez le programme de test correspondant a l'archive :

   x86 :
       .\cortex_test_target_x86.exe

   x64 :
       .\cortex_test_target_x64.exe

2. Injectez Cortex :

       .\injector.exe cortex_test_target_x86.exe

   ou :

       .\injector.exe cortex_test_target_x64.exe

3. Verifiez que l'API repond :

       Invoke-RestMethod http://127.0.0.1:6969/health

Une reponse contenant "ok": true confirme que l'agent est charge.


TOKEN ET PREMIERE REQUETE
-------------------------

Apres l'injection, Cortex cree cortex.token a cote du module charge ou dans son
repertoire de travail. Les routes protegees demandent ce token.

Exemple PowerShell :

       $token = (Get-Content .\cortex.token -Raw).Trim()
       $headers = @{ "X-Cortex-Token" = $token }
       Invoke-RestMethod http://127.0.0.1:6969/modules -Headers $headers

Les routes publiques /health, /status, /tools et /openapi.json permettent de
verifier le service et de decouvrir l'API.


DIAGNOSTIC D'UN CRASH OU D'UN FREEZE
------------------------------------

Surveillez un processus deja injecte :

       .\cortex_host.exe diagnose --pid 1234 --heartbeat render --hang-ms 5000

Analysez ensuite un dossier de rapport :

       .\cortex_host.exe analyze C:\chemin\vers\crash_...

Pour symboliser une adresse hors ligne :

       .\cortex_host.exe symbolize --image C:\mods\MonMod.dll --rva 0x1832


PROBLEMES COURANTS
------------------

"DLL failed to load" ou "bitness mismatch"
    Verifiez que le ZIP x86/x64 correspond exactement au processus cible.

"No matching process found"
    Utilisez le nom exact visible dans le Gestionnaire des taches ou le PID.

"OpenProcess failed"
    Le processus peut demander des droits administrateur ou bloquer l'injection.

L'API ne repond pas
    Verifiez que Cortex n'est pas deja charge, que le port 6969 est libre et
    consultez les fichiers de log generes a cote de Cortex.

Le client MCP indique "cortex_unreachable"
    Verifiez que Cortex est bien injecte, que cortex.token correspond au runtime
    courant et que cortex_host.exe/cortex_core.dll ont la meme architecture.

L'ASI ne se charge pas
    Verifiez l'architecture de l'ASI loader, son dossier de plugins et ses logs.


SECURITE ET UTILISATION AUTORISEE
--------------------------------

Utilisez Cortex uniquement sur des logiciels que vous possedez ou pour lesquels
vous avez une autorisation explicite. Cortex est destine au debogage, a la
recherche hors ligne, a l'accessibilite et au modding solo. Ne l'utilisez pas
pour contourner un anti-cheat ou interferer avec des services en ligne.
