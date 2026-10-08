# Fontes de PKG para Peppy Store

Conferência de 2026-10-04. A loja instala pacotes base completos de PS4;
listas de jogos e páginas de hospedagem exigem conferência do arquivo final.
Esta revisão consulta índices públicos e os arquivos selecionados para o
catálogo. Não é uma importação de todos os arquivos de cada site. No
Internet Archive, os cabeçalhos dos 867 candidatos nativos selecionados
foram processados; jogos comerciais completos não foram baixados.

| Fonte indicada | Evidência disponível | Resultado nesta integração |
| --- | --- | --- |
| [Pippo .exFAT](https://pippo26442999.github.io/.exFAT/) | 777 registros no JSON, todos com identificadores PPSA de PS5. | Nenhuma entrada para o catálogo PS4. |
| [DLPS](https://dlpsgame.com/category/ps4/) | Categoria com 20 posts por página e 324 páginas. The Rumble Fish 2 aponta para intermediários; `/archives/47074` retornou HTTP 403. | Nenhum binário verificável no exemplo consultado. |
| [PFS Library](https://pfs-library.vercel.app/) | A página indica mudança para `pfs-library.xetdy-am.workers.dev`, que retornou HTTP 404. | Catálogo indisponível no endereço indicado. |
| [SuperPSX](https://www.superpsx.com/) | Revisão adicional de 2026-10-05: espelhos públicos forneceram 6.583 links associados a 1.812 nomes. No MediaFire, 46 RARs e um patch PKG; sete candidatos a base PKG do Datanodes retornaram 404. A amostra TABS usa Viking/Turnstile e Mocha sem binário direto conferido. | Nenhum PKG instalável adicionado dessa revisão. Pesquisa e abas separadas não transformam páginas de hospedagem ou arquivos compactados em pacotes completos. Veja [superpsx-review.json](superpsx-review.json). |
| [MTPS4](https://mtps4store380.blogspot.com/?m=1) | JSON público com 2.171 entradas e 3.434 links. Há 18 URLs com nome `.pkg`: 14 páginas MediaFire, três links Akirabox assinados e um FileKeeper. A página Akirabox consultada retornou HTTP 403. | Oito pacotes base integrados via MediaFire: Agony e sete conversões de PS2 para PS4. |

O feed Blogger do MTPS4 contém uma postagem de imagens; o catálogo efetivo
é um JSON carregado pela página. A conferência usa a versão
[`cb3244e`](https://github.com/Mtgames38oficial/jogosps4pkg/blob/cb3244edfa2825627320efff0db5701f7493a163/jogosps4pkg.json).
Nomes com `.pkg` na URL não comprovam que a resposta seja um pacote, e
tamanhos arredondados da lista não substituem o tamanho real do servidor.

## Arquivos conferidos

Foram lidos apenas os primeiros 1.080 bytes de cada arquivo, com pedidos
HTTP Range. Nenhum jogo completo foi baixado durante essa revisão.

| Arquivo | Content ID | Tamanho real | Cabeçalho | Decisão |
| --- | --- | --- | --- | --- |
| Agony | `UP2047-CUSA10216_00-AGONY666AMERICAS` | 10.518.134.784 bytes | Magic `7f434e54`, tipo `0x1A`, flags `0x0A000000`. | Pacote base do catálogo. |
| Yet Another Zombie Defense HD | `UP2387-CUSA18354_00-YAZDHD0000000000` | 83.492.864 bytes | Tipo `0x1A`, flags `0x62300000` com bits de patch. | Recusado como pacote base, mesmo estando listado como “Game”. |

Nos dois exemplos, o tamanho de pacote em `0x430` coincide com o total de
`Content-Range`. Os campos são documentados no
[`PkgReader`](https://github.com/maxton/LibOrbisPkg/blob/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/PKG/PkgReader.cs)
e os tipos/flags em
[`Enums`](https://github.com/maxton/LibOrbisPkg/blob/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/PKG/Enums.cs).
O tipo `0x1A` sozinho também pode identificar um patch ou remaster.

Os outros sete pacotes conferidos têm magic `7f434e54`, tipo `0x1A`, flags
`0x0A000000` e tamanho declarado igual ao total de HTTP Range. Seus
identificadores SLES/SLUS pertencem às conversões de PS2 para PS4; não são
PKGs nativos de jogos PS4. O cabeçalho e a procedência foram revisados juntos.

| Conversão | Title ID do cabeçalho | Tamanho real em bytes |
| --- | --- | --- |
| Futurama | SLES51507 | 5.250.744.320 |
| Need for Speed: Most Wanted | SLUS21351 | 3.742.892.032 |
| Need for Speed: Carbon | SLUS21494 | 3.729.260.544 |
| Pica-Pau | SLES50612 | 443.547.648 |
| Black | SLUS21376 | 1.359.675.392 |
| Area 51 | SLES52570 | 3.543.728.128 |
| Armored Core 2: Another Age | SLUS20249 | 3.815.243.776 |

Pica-Pau aparece com SLES50613 na lista original, mas o Content ID conferido
tem Title ID SLES50612. A entrada usa o identificador do arquivo recebido.

O catálogo guarda a página estável do MediaFire. A transferência consulta
essa página novamente, lê seu botão de download e usa o endereço HTTPS do
CDN naquele momento. A página nunca é gravada como PKG. Sites com captcha,
arquivos compactados, partes e links expirados precisam de tratamento
específico antes de disponibilizar a instalação.

A conferência do cabeçalho não valida a assinatura ou todas as estruturas
internas, e não comprova instalação no firmware 13.52. A transferência
completa continua verificando tamanho, magic e SHA-256 quando disponível.

## Fontes adicionais e ampliação do catálogo

| Fonte indicada | Resultado da conferência de 2026-10-04 |
| --- | --- |
| [PKG-Zone](https://pkg-zone.com/) | Página e catálogo `api.pkg-zone.com/store.db` retornaram HTTP 500, inclusive com o User-Agent documentado da HB-Store. A HB-Store já está no catálogo por release oficial no GitHub. Os primeiros espelhos de Itemzflow e PS4-Xplorer retornaram HTTP 403; Itemzflow foi posteriormente conferido em um espelho público no GitHub, descrito abaixo. |
| [Brewology](https://brewology.com/) | A página consultada prioriza homebrews de outras plataformas. IRISMAN, webMAN MOD e CFW/HFW de PS3 não são pacotes instaláveis neste aplicativo PS4. |
| [PSX-Place](https://www.psx-place.com/resources/categories/ps4-homebrew.42/) | O recurso [ioQuake3 PS4](https://www.psx-place.com/resources/ioquake3-ps4.1717/) aponta para o autor Mayo1970. Foram incluídas cinco builds da release 1.8, com dados de jogo externos explicitados. |
| [FPKGi](https://github.com/ItsJokerZz/FPKGi/releases/tag/v1.10.0) | Aplicativo PS4/PS5 do autor, release fixa v1.10.0, incluído por pacote PS4 conferido. Esta entrada instala o cliente; suas listas de jogos não foram importadas. |
| [GameBaTo](https://gamebatoapp.ir/home/en/) | Cliente PS4 incluído a partir do link público `/home/app.pkg`. O site anuncia firmware 5.05 a 12.0, sem comprovar 13.52. A página não publica versão/checksum: o catálogo mostra “site sem versão” e fixa o SHA256 calculado nesta revisão. |
| PKGi / NoPayStation | As opções indicadas para PS3, PS Vita e PSP ficam fora do catálogo PS4. A Peppy não integra downloads de servidores da PlayStation. |
| [Vimm's Lair](https://vimm.net/vault/PS4) | O endereço PS4 consultado retornou HTTP 404. Discos, ISOs e pastas de PS3 não são PKGs base instaláveis de PS4. |
| [Internet Archive](https://archive.org/details/ps4-fpkg-collection-english-h) | As coleções alfabéticas revisadas forneceram 804 jogos nativos distintos após conferir disponibilidade, metadados, cabeçalhos e duplicatas. A fonte declara versões modificadas em inglês, com atualizações mescladas, conteúdo removido e possíveis limitações no PS4 Pro. Sete conversões de PS1/PS2 com indicação português/BR também foram integradas, em itens separados. |
| [Romsfun](https://romsfun.com/download/sonic-mania-40290) | Sonic Mania leva a uma página 1fichier com link temporário. Nenhum binário direto foi conferido; não foi incluído como PKG instalável. |
| [Romspure](https://romspure.cc/roms/sony-playstation-4/) | O exemplo Puyo Puyo Tetris anuncia PKG, mas a página consultada não fornece um arquivo direto verificável. |

As quatro novas releases oficiais iniciais são FPKGi, PS4 Cheats Manager,
NP2kai PS4 e mGBA PS4. Os arquivos vieram dos repositórios dos próprios
autores, com tag fixa; Cheats Manager não publica SHA256, enquanto as outras
três releases publicam. NP2kai e mGBA precisam de dados externos descritos
nas respectivas entradas. Firmware 13.52 permanece sem teste no console.

| Exemplos de arquivos conferidos | Title ID conferido | Tamanho em bytes |
| --- | --- | --- |
| FPKGi v1.10.0 | PKGI13337 | 85.458.944 |
| PS4 Cheats Manager v1.2.2 | CHTM00777 | 19.202.048 |
| NP2kai v1.0 | BREW00984 | 9.109.504 |
| mGBA ps4-v0.1.0 | MGBA00001 | 6.619.136 |
| ioQuake3: Quake III Arena 1.8 | QUAK03000 | 13.762.560 |
| ioQuake3: Team Arena 1.8 | QUAK03001 | 13.762.560 |
| ioQuake3: Open Arena 1.8 | QUAK03002 | 13.762.560 |
| ioQuake3: Classic 1.8 | QUAK03003 | 13.107.200 |
| ioQuake3: Elite Force 1.8 | QUAK03004 | 13.762.560 |
| GameBaTo, arquivo do site em 2026-10-04 | GBTX00001 | 23.068.672 |
| Hotline Miami, versão modificada 1.01 | CUSA00486 | 161.939.456 |
| Hotline Miami 2, versão modificada 1.01 | CUSA00368 | 391.512.064 |
| Hollow Knight, versão modificada 1.02 | CUSA13285 | 1.237.516.288 |

Todos esses cabeçalhos têm magic `7f434e54`, tipo `0x1A`, flags
`0x0A000000` e tamanho declarado igual ao total de HTTP Range. A fonte e
o formato PS4 também foram conferidos; o instalador não aceita atualizações
ou DLCs como pacotes base.

As cinco builds de ioQuake3 não incluem os arquivos `.pk3` dos jogos. O
autor documenta os diretórios em
[INSTALLATION.md da tag 1.8](https://github.com/Mayo1970/ioQuake3-PS4/blob/1.8/INSTALLATION.md).
Open Arena usa dados gratuitos, também fornecidos à parte. As outras builds
precisam dos dados correspondentes; instalar o motor sozinho não basta.

O cliente GameBaTo foi baixado integralmente para calcular SHA256:
`529a33a55722c2488c9b190da6eb6132997903c3253091e576538785d3c937bb`.
O arquivo Itemzflow do espelho também foi baixado integralmente para ler
o PARAM.SFO e calcular SHA-256, conforme a seção abaixo. Nenhum jogo
comercial completo foi baixado nesta revisão. Como `app.pkg` é mutável,
uma mudança no arquivo do servidor exige atualizar a evidência e o hash.

## Revisão das coleções nativas do Internet Archive

Os pedidos HTTP Range leem os primeiros 1.080 bytes e comparam magic,
Content ID, tipo/flags de base e tamanho declarado com o tamanho HTTP.
Os metadados fornecem nome, versão, tamanho e hashes publicados pela fonte;
MD5/SHA1 de metadados não são tratados como SHA-256 verificado do arquivo.
O resultado dos 867 candidatos nativos foi:

| Resultado | Quantidade |
| --- | ---: |
| Cabeçalho de pacote base aprovado | 808 |
| HTTP 401, sem arquivo público acessível | 44 |
| HTTP 403, acesso recusado | 13 |
| Cabeçalho recusado por flags de patch | 2 |
| Total processado | 867 |

Os dois cabeçalhos de patch pertencem a Serial Cleaner e Cyberpunk 2077.
Quatro nomes duplicados entre os 808 candidatos aprovados foram removidos,
resultando em 804 jogos nativos distintos do Archive. Agony é a outra base
nativa, obtida pelo MediaFire: 805 jogos nativos no catálogo. Aplicativos,
motores, emuladores e conversões PS1/PS2 não são usados para atingir a meta
de jogos nativos.

Respostas HTTP 500 e timeouts da rota estável foram tentados nas réplicas
públicas indicadas pelos próprios metadados. Entre 293 candidatos encaminhados
para essa recuperação, 292 foram recuperados. Esses casos usam a URL HTTPS
do CDN efetivamente conferido; nenhuma proteção de acesso foi contornada.
Arquivos com HTTP 401/403 permaneceram excluídos.

As 27 coleções e os 57 servidores CDN aprovados ficam em
[`archive_sources.json`](archive_sources.json). Essa lista é compartilhada:
o gerador Python lê o JSON e
[`scripts/archive_policy.py`](scripts/archive_policy.py) gera o cabeçalho
C++ do downloader. A política aceita somente nomes exatos de coleções e
hosts observados, com rota de um único arquivo `.pkg`; não usa wildcards de
domínio. Um novo CDN exige nova revisão. Cada provedor permanece isolado
nos redirecionamentos HTTPS, e nenhuma rota de servidor Sony foi integrada.

A disponibilidade é a observada em 2026-10-04. Essa revisão do cabeçalho
não comprova assinatura, integridade de todos os dados, execução no PS4
Pro ou compatibilidade com firmware 13.52. O downloader confere novamente
tamanho, identidade, tipo e SHA-256 quando existe um hash esperado antes
de encaminhar o arquivo recebido ao instalador.

## Português e conversões de jogos clássicos

A pesquisa de bases nativas em português encontrou MediEvil, Deadpool e
Shadow Complex Remastered com afirmações de idioma, porém somente em
arquivos `.7z`. Esses arquivos não foram incorporados como PKGs diretos.
Uma tradução de fã oferecida à parte também não transforma a base original
em um pacote português. A coleção nativa em inglês permanece identificada
como inglês; região europeia não é usada como prova de idioma.

Foram integrados sete PKGs de clássicos com indicação de português/BR na
fonte. São conversões para PS4, explicitadas como PS1 ou PS2 na loja:

| Conversão | Title ID do cabeçalho | Tamanho real em bytes |
| --- | --- | ---: |
| God of War: O Bom de Guerra (PS2) | SCUS97399 | 3.279.749.120 |
| Parasite Eve II (PS1, PT-BR) | SLUS01042 | 976.355.328 |
| Resident Evil (PS1, BR) | CUSA00927 | 411.697.152 |
| Resident Evil 2 (PS1, BR) | CUSA00755 | 944.766.976 |
| Resident Evil Survivor (PS1, BR) | CUSA01088 | 245.760.000 |
| Silent Hill (PS1, BR) | CUSA00925 | 397.803.520 |
| Yu-Gi-Oh! Forbidden Memories (PS1, BR) | CUSA14511 | 248.578.048 |

Esses cabeçalhos têm magic `7f434e54`, tipo `0x1A`, flags `0x0A000000` e
tamanho declarado igual ao total HTTP. Os identificadores CUSA em algumas
conversões não mudam a plataforma original dos jogos: a procedência informa
PS1. O idioma é uma indicação do nome/descrição da fonte, sem teste de áudio
ou legendas no console. Com as sete conversões anteriores do MediaFire,
são 14 conversões, separadas dos 805 jogos nativos. A conversão de Resident
Evil 3: Nemesis também teve cabeçalho conferido, mas usa `CUSA00924`, o
mesmo Title ID da base nativa Resident Evil Revelations 2; ela foi excluída
para evitar colisão entre os aplicativos.

## Itemzflow de espelho não oficial

[Niklas080208/ps4-aio-apps](https://github.com/Niklas080208/ps4-aio-apps/blob/bf32bd6cf497ee9070f3f2f962c63ed19330e8bc/apps.json)
oferece um espelho público de Itemzflow. O arquivo de 27.131.904 bytes foi
fixado pelo commit `bf32bd6cf497ee9070f3f2f962c63ed19330e8bc`, conferido
integralmente e rotulado como **espelho não oficial**. O PARAM.SFO informa
`TITLE=Itemzflow Game Manager`, `TITLE_ID=ITEM00001` e `APP_VER=01.08`;
a versão exibida vem do arquivo, pois o manifesto não a informa.

SHA-256 calculado:
`acd4f37aad8d686a433e73f1b44a3e87b513c10ee7303ff965cb3dc54048e60e`.
O hash fixa os bytes desse espelho, sem autenticar o arquivo pelo autor
original. Funcionamento em 13.52 ainda precisa de teste no console.

## Catálogo consolidado

O catálogo gratuito contém 860 entradas: 805 jogos nativos, 14 conversões PS1/PS2
e 41 aplicativos, emuladores e motores. Por procedência, são 39 releases
oficiais, oito pacotes MediaFire, 811 pacotes Internet Archive, o cliente
GameBaTo do site e Itemzflow de um espelho não oficial.

Não é uma importação completa dos sites. Nenhum PKG enviado pelo usuário foi
incluído nesta revisão, pois os anexos recebidos até agora são fotografias.


## Expansão premium e pesquisa de outubro

A expansão é mantida no catálogo remoto do serviço Peppy. Os itens já existentes continuam livres; duplicatas, como Friday Night Funkin, não foram cobradas nem contadas como adições. `premium-catalog-review.json` registra o novo escopo e o seed público de metadados está em `../services/peppy-hub/seed-catalog.json`. Cada pacote incluído deve ter tamanho, Content ID, tipo e flags consistentes com o cabeçalho; links de sites e arquivos RAR não são tratados como downloads diretos.

A busca em Sandro Store, Infinity, GameBaTo e fontes da comunidade não autoriza acesso a catálogos fechados. Metadados recebidos por administração são validados antes de publicação e novamente pelo cliente; procedência conhecida não prova licenciamento ou funcionamento no firmware. Português continua prioritário quando documentado, sem transformar nomes de arquivos em comprovação de dublagem.
