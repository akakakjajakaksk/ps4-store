# Orbis Store Native 0.1

Versao nativa experimental da Orbis Store para PlayStation 4 homebrew.

## Objetivo
- App separado da versao web
- Navegacao por DualShock 4
- Catalogo de homebrews autorizados
- Base preparada para rede/download em etapas posteriores

## Toolchain
Projeto estruturado para OpenOrbis PS4 Toolchain.

## Estado 0.1
A primeira build mostra a interface inicial nativa e responde ao controle. A instalacao automatica de PKG ainda NAO esta implementada; ela so sera marcada como pronta depois de validacao real no PS4/GoldHEN.

## Build
Instale o OpenOrbis PS4 Toolchain e configure OO_PS4_TOOLCHAIN. Depois use make dentro desta pasta.

A versao web da Orbis continua independente na raiz do repositorio.


## Native 0.3
- Renderer fullscreen 1920x1080 via SDL2
- Faixa de destaque e biblioteca horizontal
- Cards com foco visual
- Navegacao esquerda/direita pelo controle
- Estrutura visual sem depender do navegador do PS4

Observacao: esta etapa e um prototipo grafico de codigo-fonte. Precisa ser compilada com a distribuicao do OpenOrbis que inclua SDL-PS4 e validada no hardware antes de ser chamada de build funcional.
