CORTEX v0.7.0 - INSTALLATION ET DEMARRAGE RAPIDE
================================================

Cortex v0.7.0 est distribue comme une seule application Windows portable :
cortex.exe. Les composants d'instrumentation x64 et x86 sont inclus dans le
sous-dossier runtime et sont selectionnes automatiquement selon la cible.

INSTALLATION
------------

1. Telechargez l'archive :

       cortex-v0.7.0-windows-portable.zip

2. Decompressez toute l'archive dans un dossier normal et inscriptible,
   par exemple :

       C:\Cortex\

3. Ne deplacez pas cortex.exe seul. Conservez les DLL Qt, les plugins QML et
   le dossier runtime a cote de l'executable.

4. Lancez :

       .\cortex.exe

PREMIERE SESSION
----------------

1. Selectionnez un processus dans la barre superieure.
2. Attachez Cortex a la cible.
3. Utilisez Scanner (Ctrl+F) ou Memory pour commencer l'inspection.
4. Double-cliquez un resultat de scan utile pour l'ajouter a Addresses.
5. Utilisez le menu contextuel d'une adresse pour naviguer vers Memory,
   Disassembly, RE, Pointers, Structures ou les actions de debugger.
6. Activez Mutation uniquement lorsque vous voulez effectuer une operation
   qui modifie la cible ou l'etat persistant.

MODES EN LIGNE DE COMMANDE
--------------------------

Afficher l'aide :

       .\cortex.exe --help

Afficher la version :

       .\cortex.exe --version

Lancer MCP en mode stdio persistant :

       .\cortex.exe mcp

Attacher automatiquement MCP a un PID :

       .\cortex.exe mcp --pid 1234

Attacher automatiquement MCP a un processus :

       .\cortex.exe mcp --process game.exe

Exposer aussi les outils primitifs bas niveau :

       .\cortex.exe mcp --tools all

Autres commandes integrees :

       .\cortex.exe probe --pid 1234
       .\cortex.exe diagnose --pid 1234
       .\cortex.exe analyze <dossier>
       .\cortex.exe symbolize [options]

CONTENU IMPORTANT
-----------------

cortex.exe
    Application Cortex principale (GUI et modes CLI/MCP).

runtime\x64\cortex_core.dll
    Payload d'instrumentation pour les cibles 64 bits.

runtime\x86\cortex_core.dll
    Payload d'instrumentation pour les cibles 32 bits.

runtime\x86\cortex_runtime_helper.exe
    Helper prive utilise automatiquement pour les cibles 32 bits.

README.md, CHANGELOG.md, LICENSE, docs
    Documentation, historique, licence et guides techniques.

DEPANNAGE
---------

- Si cortex.exe ne demarre pas, verifiez que l'archive a ete entierement
  extraite et que les DLL/plugins fournis sont toujours presents.
- Si une cible ne peut pas etre ouverte, verifiez son architecture et les
  droits Windows du processus. N'utilisez des privileges eleves que lorsque
  les droits de la cible l'exigent.
- Si une operation d'ecriture, de breakpoint ou de controle est refusee,
  activez explicitement Mutation dans Cortex.
- Pour MCP, stdout est reserve au protocole JSON-RPC pendant toute la session.
- Consultez docs/getting-started.md et docs/ui-guide.md pour le workflow
  complet de l'application unifiee.

AUTORISATION
------------

Utilisez Cortex uniquement sur des logiciels et systemes que vous possedez
ou etes autorise a inspecter. Le contournement d'anti-cheat, l'acces non
autorise et l'interference avec des services en ligne ne font pas partie du
perimetre du projet.
