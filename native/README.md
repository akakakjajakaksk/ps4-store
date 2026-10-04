# Peppy Store Native

Interface nativa para PS4 homebrew, compilada com OpenOrbis PS4 Toolchain.

## Interface

- Tela em 1920×1080 com fundo escuro e destaques azuis.
- Arte PEPPY no banner e no ícone do menu do PS4.
- Fonte suave com caracteres em português, preparada durante a build.
- Catálogo de 15 PKGs, com categorias, páginas e detalhes.
- Downloads HTTPS de releases oficiais, em segundo plano.

O catálogo está em `catalog.json`, com versões, URLs fixas, tamanhos e
evidências de firmware verificadas em 2026-10-04. Inclui utilitários,
emuladores, PS4 Media Player e dois pacotes de Freedoom. O catálogo não
representa todos os aplicativos existentes para PS4.
A interface e os downloads não se conectam a servidores da PlayStation.

O download verifica tamanho e assinatura de PKG, e confere SHA-256 quando
o autor publica um hash. Pacotes concluídos ficam em
`/data/peppy-store/downloads/`. Arquivos parciais são removidos em caso de
erro ou cancelamento. A instalação deve ser feita com um instalador de PKG
compatível; esta versão não inicia a instalação automaticamente.

A conexão HTTPS valida os certificados e pode falhar caso o relógio,
a rede ou os certificados do console sejam incompatíveis. O downloader
foi testado com HTTP simulado e precisa de teste de download no PS4.

A inicialização consulta o estado da rede/IP via NetCtl e aceita módulos
já carregados após verificar seu estado. Falhas mostram a etapa e o retorno
nativo, além do código resumido. O diagnóstico fica em
`/data/peppy-store/downloads/download.log`, sem URLs nem tokens de redirecionamento.

## Controles

- Esquerda/direita: selecionar um card, com retorno ao início/fim da lista.
- L1/R1: trocar a categoria.
- X: abrir os detalhes; na tela de detalhes, baixar o PKG.
- Bolinha: voltar à biblioteca.
- Triângulo: cancelar o download atual.

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
python3 native/scripts/generate-catalog.py --catalog native/catalog.json --output native/ui_catalog.h
g++ -std=c++11 -O2 native/scripts/preview-ui.cpp -o /tmp/peppy-preview
/tmp/peppy-preview /tmp/peppy
```

O programa gera arquivos PPM para biblioteca e detalhes de todos os itens
e categorias, além dos estados de download, usando o renderizador nativo.

Os testes de transferência em `tests/downloads/` também são executados na
workflow antes da compilação nativa. Veja o README daquela pasta para rodar
os testes no host.

O funcionamento no console depende de teste real no PS4/GoldHEN.
