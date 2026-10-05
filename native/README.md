# Peppy Store Native

Interface nativa para PS4 homebrew, compilada com OpenOrbis PS4 Toolchain.

## Interface

- Tela em 1920×1080 com fundo escuro e destaques azuis.
- Arte PEPPY no banner e no ícone do menu do PS4.
- Fonte suave com caracteres em português, preparada durante a build.
- Catálogo de 845 PKGs, com categorias, páginas e detalhes.
- Downloads HTTPS de releases oficiais, MediaFire e fontes diretas revisadas, em segundo plano.
- Instalação local integrada após concluir e validar o download.
- Playlist FIGHT → ACENDAOFAROL, com opção para silenciar e trocar a faixa.

O catálogo está em `catalog.json`, com versões, fontes, tamanhos exatos e
evidências de arquivo conferidas em 2026-10-04. São 805 jogos nativos de PS4,
14 conversões de PS1/PS2 e 26 aplicativos, emuladores e motores de jogos.
Os jogos nativos incluem Agony e 804 títulos distintos do Internet Archive;
as conversões não entram nessa contagem. As entradas identificam a fonte e
distinguem as conversões dos jogos nativos.

A coleção nativa do Archive informa versões em inglês, atualizações
mescladas e conteúdo removido; essas versões não são apresentadas como
cópias sem alterações nem como jogos em português. Sete conversões novas
têm indicação de português/BR no nome ou na descrição da fonte. Essa
indicação não foi confirmada jogando no console. A pesquisa de bases nativas
em português encontrou arquivos compactados, sem PKG direto aprovado.
O catálogo também inclui Itemzflow de um espelho não oficial fixado por
commit e SHA-256. O catálogo não representa todos os aplicativos existentes
para PS4, e os cabeçalhos não comprovam funcionamento no firmware 13.52.
A interface e os downloads não se conectam a servidores da PlayStation.

O download verifica tamanho e magic de PKG, e confere SHA-256 quando
há um hash revisado. A magic não valida a assinatura criptográfica do pacote. Pacotes concluídos ficam em
`/data/peppy-store/downloads/`. Arquivos parciais são removidos em caso de
erro ou cancelamento. A ação **Baixar e instalar** encaminha o pacote validado
para AppInstUtil/BGFT, o instalador do PS4, sem precisar abrir outro aplicativo.
A loja acompanha a tarefa e só informa sucesso após confirmar a instalação.
Uma falha permite tentar instalar novamente o arquivo já baixado.
Download e instalação usam o mesmo limite de 256 GiB por pacote, com tamanhos
e progresso de 64 bits. A interface mostra GB a partir de 1 GiB. O conteúdo
continua sendo transferido em blocos pequenos; o limite não reserva essa
quantidade de memória. A transferência usa um buffer de leitura de 256 KiB
na heap e outro de 256 KiB para a gravação, fornecido explicitamente a
`setvbuf`. Esses dois buffers somam 512 KiB fixos, além dos recursos de rede.
O SHA-256 só é calculado quando existe um hash esperado no catálogo; nesse
caso a comparação continua obrigatória. Isso reduz trabalho sem remover as
verificações de tamanho, identidade e tipo. Não foi medida a velocidade real
de transferência no PS4. Os testes de pacotes grandes usam respostas simuladas
e arquivos esparsos, sem comprovar uma transferência completa no console.

Quando o HTTP informa `Content-Length`, esse tamanho precisa coincidir com
o tamanho do catálogo. A leitura termina assim que recebe essa quantidade,
sem esperar outra leitura ou o fechamento da conexão. Content ID, tipo/flags,
tamanho e SHA-256 disponível continuam sendo verificados antes de publicar
o arquivo e iniciar a instalação. A leitura extra após 100% podia retornar
o timeout `0x80431068` e descartar uma transferência já completa.
Respostas sem tamanho HTTP conhecido ainda precisam terminar por EOF e
recusam bytes além do tamanho esperado. A mesma regra de conclusão pelo
tamanho declarado vale para a página MediaFire, com limite de 1 MiB.
Esse comportamento segue o [RFC 9112, §6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3)
e é coberto por respostas simuladas; o teste final continua sendo no PS4.

O instalador abre os arquivos com `O_NOFOLLOW` e verifica o descritor com
`sceKernelFstat`, usando a estrutura nativa de 120 bytes. A chamada `lstat`
da biblioteca musl para PS4 não está implementada
e retornava `ENOSYS` (`0x4E`) antes de iniciar a instalação.

A integração consulta o SDK público do GoldHEN, registrando o retorno e o
`errno` imediatamente. Com a interface 1.00 disponível, usa os comandos
oficiais de permissões para instalar por arquivo global e restaura o contexto
original ao terminar. Se essa interface não estiver disponível, tenta o
instalador nativo com as permissões atuais, por HTTP em `127.0.0.1`.
Esse servidor temporário fornece o PKG já validado pelo descritor aberto,
com suporte a intervalos de bytes. Ele não aceita conexões de outros aparelhos
e termina antes de fechar o arquivo. Não é necessário baixar o PKG novamente.

Os pedidos do instalador podem conter parâmetros após `?`. O servidor compara
o caminho exato do pacote separadamente desses parâmetros, tanto em pedidos
com caminho relativo quanto com URL HTTP completa do próprio endereço local.
O caminho, a porta e o arquivo continuam limitados ao PKG desta instalação.
Recusar esses parâmetros causava uma resposta HTTP 404; a implementação
[SSPI](https://github.com/Xyhlo/SSPI/blob/43247239733132b3e373c6ea9db3553b59da6413/app/LoopbackPkgServer.cs)
documenta a mesma associação com o erro BGFT `0x80991404` observado no console.
O diagnóstico de falha e `install.log` incluem número de pedidos, última
resposta HTTP e bytes do PKG enviados, sem registrar URLs ou parâmetros.

O caminho alternativo não depende de offsets de kernel nem altera permissões.
O serviço nativo ainda pode recusar a instalação; nesse caso a loja mostra a
etapa e o código real e mantém o PKG. Ela não substitui aplicativos já
instalados nem a si mesma. Uma falha de encerramento exige reabrir a loja
antes de continuar.
O log de instalação fica em `/data/peppy-store/downloads/install.log`.

A conexão HTTPS valida os certificados e pode falhar caso o relógio,
a rede ou os certificados do console sejam incompatíveis. O downloader
foi testado com HTTP simulado e precisa de teste de download no PS4.

Para MediaFire, a loja guarda a página do arquivo e resolve novamente o
botão de download a cada tentativa. O HTML é limitado a 1 MiB, com HTTPS,
certificados e redirecionamentos verificados; páginas que exigem captcha ou
não fornecem um botão válido mostram erro de fonte. O HTML não é salvo como
pacote nem entra no progresso. A transferência aceita somente páginas do
MediaFire e seu CDN `download<dígitos>.mediafire.com` neste fluxo.
Os pacotes externos também conferem Content ID, tipo/flags de base e tamanho
declarado no cabeçalho antes de publicar o arquivo completo. O catálogo
guarda evidências de cabeçalho e HTTP Range; isso não comprova assinatura,
todas as estruturas internas ou instalação no console.
Internet Archive usa as rotas das coleções revisadas e somente os hosts
HTTPS observados nesta conferência. `archive_sources.json` é a lista
autoritativa de 27 coleções e 57 servidores CDN: o gerador Python lê esse arquivo e
`scripts/archive_policy.py` gera `archive_sources.h` para o downloader nativo.
Não há autorização por wildcard. Se o serviço mudar para outro CDN, o
download será recusado até uma nova revisão; redirecionamentos não podem
trocar de provedor. GameBaTo usa somente `/home/app.pkg` no site conferido,
com SHA256 calculado em 2026-10-04 para fixar o arquivo mutável.
O site anuncia firmware 5.05 a 12.0; funcionamento em 13.52 não está confirmado.
Instalar FPKGi ou GameBaTo não importa seus catálogos para a Peppy nem
comprova quais servidores serão usados pelos clientes externos.
Os resultados das fontes indicadas estão em [sources-review.md](sources-review.md).

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

## PKGs enviados e novas fontes

Para conferir um arquivo completo antes de revisar sua entrada no catálogo:

```sh
python3 native/scripts/inspect-pkg.py "/caminho/arquivo.pkg" --output report.json
```

O relatório contém tamanho exato, Content ID, SHA-256 e um nome ASCII sugerido,
preservando o nome original. O inspetor recusa arquivos compactados, arquivos
alterados durante a leitura e identificadores PPSA de PS5. Um identificador
desconhecido exige comprovação da plataforma; o cabeçalho não comprova que o
pacote está completo, é um pacote base ou instala no firmware do console.

O arquivo local ainda precisa de um endereço de download acessível ao PS4.
A opção `--url` registra uma URL HTTPS fornecida, validando apenas sua sintaxe.
Ela não envia o arquivo nem verifica o servidor. Colocar um relatório no
repositório não hospeda o PKG. Arquivos grandes precisam de hospedagem
adequada a seu tamanho.

Uma entrada precisa apontar para o PKG completo, com tamanho verificado e
origem identificada. Páginas de download, arquivos RAR/ZIP, partes de um
arquivo, captchas e links de PS5 não são downloads diretos instaláveis pela
loja. Atualizações e DLCs também precisam de um fluxo de instalação próprio.
O catálogo preserva as 15 releases anteriores e contém 24 releases oficiais,
oito pacotes MediaFire, 811 pacotes do Internet Archive, o cliente GameBaTo
conferido pelo site do fornecedor e Itemzflow de um espelho não oficial.
Os 811 pacotes do Archive são 804 jogos nativos e sete conversões em
português/BR; 808 candidatos nativos aprovados foram reduzidos a 804 nomes
distintos. Outras fontes só entram após conferir formato, URL e servidores
de redirecionamento. O gerador valida a procedência e as evidências externas,
e a loja repete a verificação de identidade e tipo no arquivo recebido.

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

Os testes de transferência, instalação, entrega local, música e inspeção em `tests/downloads/`,
`tests/install/`, `tests/pkg_server/`, `tests/music/` e `tests/catalog_import/` também são executados na workflow antes da
compilação nativa. Os mocks verificam controle e falhas; instalação e áudio
reais ainda precisam de teste no PS4 com o GoldHEN ativo.

Fontes da integração: [GoldHEN SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK),
[ABI BGFT](https://github.com/flatz/ps4_stub_lib_maker_v2/blob/master/include/bgft.h)
e instalação por HTTP local em
[ezRemote](https://github.com/cy33hc/ps4-ezremote-client/blob/master/source/installer.cpp#L933),
com [resposta binária e intervalos](https://github.com/cy33hc/ps4-ezremote-client/blob/master/source/server/http_server.cpp#L1046).

O funcionamento no console depende de teste real no PS4/GoldHEN.
