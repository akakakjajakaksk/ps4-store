# Peppy Store Native

Interface nativa para PS4 homebrew, compilada com OpenOrbis PS4 Toolchain.

## Interface

- Tela em 1920×1080 com fundo escuro e destaques azuis.
- Arte PEPPY no banner e no ícone do menu do PS4.
- Fonte suave com caracteres em português, preparada durante a build.
- Quatro cards de catálogo e páginas de detalhes.

Os downloads e a instalação de aplicativos ainda não estão implementados.
As páginas de detalhes indicam que o download está indisponível.
A interface não se conecta a servidores da PlayStation.

## Controles

- Esquerda/direita: selecionar um card, com retorno ao início/fim da lista.
- X: abrir os detalhes do card selecionado.
- Bolinha: voltar à biblioteca.

## Build

Configure `OO_PS4_TOOLCHAIN` para o OpenOrbis v0.5.4. Em Linux, instale
`lld`, `python3-pil` e `fonts-dejavu-core`. Execute `make` nesta pasta.
O `ui_assets.h` é gerado automaticamente a partir das fontes e do ícone
`../peppy-icon0-1.png`; as imagens e as fontes não precisam de dependências
adicionais no console.

A workflow `.github/workflows/build-native.yml` prepara `right.sprx` e
`sce_sys/icon0.png`, compila, empacota e valida o PKG. A licença da fonte
acompanha o pacote em `assets/FONT_LICENSE.txt`.

## Prévia sem PS4

Na raiz do repositório:

```sh
python3 native/scripts/generate-ui-assets.py --icon peppy-icon0-1.png --output native/ui_assets.h
g++ -std=c++11 -O2 native/scripts/preview-ui.cpp -o /tmp/peppy-preview
/tmp/peppy-preview /tmp/peppy
```

O programa gera arquivos PPM para os quatro estados da biblioteca e os
quatro estados de detalhes usando o mesmo renderizador da build nativa.

O funcionamento no console depende de teste real no PS4/GoldHEN.
