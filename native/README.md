# Peppy Store Native

Interface nativa para PS4 homebrew, compilada com OpenOrbis PS4 Toolchain.

## Interface

- Tela em 1920×1080 com fundo escuro e destaques azuis.
- Arte PEPPY no banner e no ícone do menu do PS4.
- Fonte suave com caracteres em português, preparada durante a build.
- Catálogo de 15 PKGs, com categorias, páginas e detalhes.
- Downloads HTTPS de releases oficiais, em segundo plano.
- Instalação local integrada após concluir e validar o download.
- Playlist FIGHT → ACENDAOFAROL, com opção para silenciar e trocar a faixa.

O catálogo está em `catalog.json`, com versões, URLs fixas, tamanhos e
evidências de firmware verificadas em 2026-10-04. Inclui utilitários,
emuladores, PS4 Media Player e dois pacotes de Freedoom. O catálogo não
representa todos os aplicativos existentes para PS4.
A interface e os downloads não se conectam a servidores da PlayStation.

O download verifica tamanho e assinatura de PKG, e confere SHA-256 quando
o autor publica um hash. Pacotes concluídos ficam em
`/data/peppy-store/downloads/`. Arquivos parciais são removidos em caso de
erro ou cancelamento. A ação **Baixar e instalar** encaminha o pacote validado
para AppInstUtil/BGFT, o instalador do PS4, sem precisar abrir outro aplicativo.
A loja acompanha a tarefa e só informa sucesso após confirmar a instalação.
Uma falha permite tentar instalar novamente o arquivo já baixado.
O instalador abre os arquivos com `O_NOFOLLOW` e verifica o descritor com
`sceKernelFstat`, usando a estrutura nativa de 120 bytes. A chamada `lstat`
da biblioteca musl para PS4 não está implementada
e retornava `ENOSYS` (`0x4E`) antes de iniciar a instalação.

A integração exige o SDK público do GoldHEN, versão 1.00. A loja consulta essa
versão antes de usar os comandos oficiais de permissões e restaura o contexto
original ao terminar. Ela não substitui aplicativos já instalados nem a si mesma.
Se o SDK ou o serviço nativo não estiver disponível, mostra o código da falha
e mantém o PKG. Uma falha de encerramento exige reabrir a loja antes de continuar.
O log de instalação fica em `/data/peppy-store/downloads/install.log`.

A conexão HTTPS valida os certificados e pode falhar caso o relógio,
a rede ou os certificados do console sejam incompatíveis. O downloader
foi testado com HTTP simulado e precisa de teste de download no PS4.

A inicialização consulta o estado da rede/IP via NetCtl e aceita módulos
já carregados após verificar seu estado. Falhas mostram a etapa e o retorno
nativo, além do código resumido. O diagnóstico fica em
`/data/peppy-store/downloads/download.log`, sem URLs nem tokens de redirecionamento.

As respostas HTTP têm um limite explícito de 64 KiB para cabeçalhos,
configurado antes de enviar cada pedido e aplicado também ao parser de
redirecionamentos. Isso acomoda os cabeçalhos e os links assinados das
releases do GitHub; o limite nativo padrão causava `0x80431073`
(`TOO_LARGE_RESPONSE_HEADER`) no teste do console.

As músicas são preparadas a partir de `fight-ps4.mp3` e `acendaofarol-ps4.mp3`
existentes na raiz do repositório. A build converte as faixas para PCM16 estéreo
a 48 kHz; os originais permanecem intactos. A reprodução usa um buffer fixo
em uma thread, volume inicial de 30% e repete as duas faixas. Os arquivos ficam
abertos durante a reprodução, inclusive enquanto o instalador muda o contexto
de acesso a arquivos. A saída MAIN usa o usuário SYSTEM (`0xFF`), como no
exemplo oficial do OpenOrbis; o usuário do perfil continua sendo usado para
o controle. Usar o perfil na saída de áudio retornava `0x809B0001`
(usuário inválido) no console. Falhas de áudio não impedem usar a loja.

## Controles

- Esquerda/direita: selecionar um card, com retorno ao início/fim da lista.
- L1/R1: trocar a categoria.
- X: abrir os detalhes; na tela de detalhes, baixar e instalar ou repetir uma instalação que falhou.
- Bolinha: voltar à biblioteca.
- Triângulo: cancelar o download ou a instalação atual.
- Quadrado: silenciar/reativar a música.
- L3: próxima faixa.

## Build

Configure `OO_PS4_TOOLCHAIN` para o OpenOrbis v0.5.4. Em Linux, instale
`lld`, `python3-pil`, `fonts-dejavu-core` e `ffmpeg` (inclui `ffprobe`). Execute `make` nesta pasta.
O `ui_assets.h` é gerado automaticamente a partir das fontes e do ícone
`../peppy-icon0-1.png`; as imagens e as fontes não precisam de dependências
adicionais no console.

A workflow `.github/workflows/build-native.yml` prepara `right.sprx` e
`sce_sys/icon0.png`, compila, empacota e valida o PKG. A licença da fonte
acompanha o pacote em `assets/FONT_LICENSE.txt`; a licença do bridge do SDK
GoldHEN acompanha `assets/GOLDHEN_SDK_LICENSE.txt`.

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

Os testes de transferência, instalação e música em `tests/downloads/`,
`tests/install/` e `tests/music/` também são executados na workflow antes da
compilação nativa. Os mocks verificam controle e falhas; instalação e áudio
reais ainda precisam de teste no PS4 com o GoldHEN ativo.

Fontes da integração: [GoldHEN SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK),
[ABI BGFT](https://github.com/flatz/ps4_stub_lib_maker_v2/blob/master/include/bgft.h)
e instalação local em [ezRemote](https://github.com/cy33hc/ps4-ezremote-client).

O funcionamento no console depende de teste real no PS4/GoldHEN.
