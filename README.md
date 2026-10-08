# Peppy Store

Loja nativa de PKGs para PS4 homebrew, com interface em português, visual preto e azul, arte Peppy, controle DualShock 4 e música FIGHT → ACENDAOFAROL. O aplicativo usa OpenOrbis e o instalador do console; seus downloads não usam servidores da PlayStation.

- [Peppy Assets Updater](https://peppy-assets-updater.quick-chime-0602.chatgpt.site): página dedicada ao download da versão publicada da loja.
- [Aplicativo nativo: instalação, controles, build e limites](native/README.md).
- [Peppy Hub: API, contas, publicação e implantação](services/peppy-hub/README.md).

## Catálogos e funções

O catálogo gratuito incorporado mantém **860 itens**: 805 jogos nativos de PS4, 14 conversões de PS1/PS2 e 41 aplicativos, emuladores e motores de jogos. Esses itens continuam gratuitos. Novas entradas da central usam o acesso premium; a quantidade disponível depende do catálogo publicado pelo administrador. Consulte as [fontes revisadas](native/sources-review.md), o [relatório do catálogo gratuito](native/catalog-review.json) e a [revisão das novas entradas premium](native/premium-catalog-review.json). O [catálogo inicial da central](services/peppy-hub/seed-catalog.json) reúne os pacotes revisados para publicação pelo administrador.

A loja oferece pesquisa por nome e Content ID/CUSA, categorias para bases, atualizações, DLCs e temas, detalhes dos pacotes, progresso, velocidade medida e instalação após o download. Pacotes completos ficam em `/data/peppy-store/downloads/` e podem ser reinstalados após uma falha de instalação. O menu **Options → Serviços** reúne links pessoais, login premium, sincronização, LivePix, atualizações da loja e os PKGs recebidos por FTP.

O premium libera o catálogo remoto publicado, incluindo entradas de mídia e temas quando disponíveis. A aba **+18 premium** exige acesso premium e confirmação de idade para entradas marcadas pelo administrador. As abas não indicam que todos os streamings, temas ou jogos solicitados já estejam disponíveis; o premium da Peppy também não substitui assinaturas de serviços de streaming. A arte Peppy aparece na interface, sem modificar os PKGs ou os créditos dos autores.

## Seus links de PKG

A importação de links pessoais funciona tanto no acesso normal quanto no premium. Use o teclado do controle em **Serviços → Importar link PKG**, ou coloque uma lista de URLs em `/data/peppy-store/urls.txt` e importe o arquivo pelo menu. A biblioteca pessoal aceita até **1.024 entradas**, permanece no console e separa os pacotes em **Base do usuário**, **Update do usuário** e **DLC do usuário**.

O importador lê metadados por HTTP Range e fixa tamanho, identidade e tipo do pacote antes do download completo. Nome e versão dependem dos metadados disponíveis. Ele aceita PKGs diretos dos provedores e caminhos revisados no aplicativo; páginas de sites, RARs, links encurtados e domínios arbitrários exigem outro fluxo. Um cabeçalho válido ou uma classificação de origem não comprova licença, idioma, assinatura ou funcionamento no firmware 13.52. Veja a [revisão do importador](native/user-catalog-review.md).

## Premium, administração e LivePix

As contas são criadas e gerenciadas pelo administrador dentro do PKG da Peppy. Senhas ficam como hashes no banco privado do serviço; o PKG não contém listas de usuários ou senhas. O aplicativo mantém a sessão em memória e o servidor confere função e validade do acesso. O catálogo sincroniza após o login e, quando a loja está ociosa, a cada cinco minutos. A sessão é conferida separadamente a cada cinco segundos, inclusive durante downloads. Invalidar uma conta ou trocar sua senha encerra todas as sessões no servidor; reativação exige novo login. Após 30 segundos sem conseguir confirmar a sessão, o acesso premium é encerrado até um novo login. Publicações recebidas não remapeiam pacotes durante um download ou uma instalação.

| Opção | Valor |
| --- | --- |
| Doação | A partir de R$ 1 |
| Premium por 15 dias | R$ 10 |
| Premium por um mês | R$ 20 |
| Premium por dois meses | R$ 30 |

Os planos usam o valor exato; os meses seguem o calendário. Pague em **[livepix.gg/peppystore](https://livepix.gg/peppystore)** e envie o comprovante no Discord para **djdarknes.com_66953**. O administrador confere o pagamento e libera ou renova o acesso manualmente; a loja não confirma pagamentos automaticamente.

O atalho **R2 + R3 + Options**, na tela de login premium, revela o login de administrador. Ele não concede permissão: criação de contas e publicação de catálogo exigem uma sessão administrativa validada pelo servidor.

No painel ADM, preencha o usuário e uma senha de pelo menos 8 bytes, escolha o plano e selecione **Criar usuário premium**. A loja informa dados inválidos ou uma tarefa ainda ocupada e mantém a senha quando o envio não puder iniciar. Após o servidor confirmar a criação, a lista de contas abre e seleciona a nova conta; pressione **X** para acessar **Invalidar usuário e senha**. Enquanto a lista estiver aberta e o aplicativo estiver disponível para consultar, ela se atualiza a cada cinco segundos. A seleção acompanha o ID da conta durante essas atualizações.

A pessoa pode entrar com as credenciais assim que a criação for confirmada, usando qualquer instalação da Peppy conectada ao mesmo serviço. Criação e invalidação são gravadas no servidor e não exigem publicar outro PKG. A invalidação bloqueia novas autenticações no servidor imediatamente; sessões já abertas no PS4 conferem a alteração a cada cinco segundos, além do tempo da rede.

## Downloads e atualizações

As transferências usam buffers limitados, preferindo 1 MiB e recuando para 256 KiB se faltar memória. PKGs grandes de fontes aprovadas podem usar duas conexões quando o servidor fornece intervalos e um ETag forte; caso contrário, a loja usa a transferência comum. Tamanho, tipo, Content ID e SHA-256, quando informado, continuam sendo conferidos. A velocidade depende do servidor e da conexão do PS4; os testes não medem o desempenho real do console. Consulte o [relatório de desempenho](native/download-performance-review.json).

O Peppy Assets Updater contém apenas o botão direto para baixar a loja e as instruções de instalação. Login, premium, LivePix e administração ficam no aplicativo do PS4. O botão de download funciona sem JavaScript. Uma atualização da Peppy é um **PKG**, não um payload: baixar pelo navegador não instala o aplicativo. Feche a loja e use o instalador do console para aplicar a atualização. Compatibilidade no firmware 13.52 precisa de teste no PS4.

## Compilar e testar

Para o aplicativo, configure `OO_PS4_TOOLCHAIN` com o **OpenOrbis v0.5.4** e instale as dependências descritas no [guia nativo](native/README.md). A build prepara arte, fonte, catálogo e áudio antes de empacotar:

```sh
make -C native
```

Para testar e compilar a central, use **Node.js 24 ou posterior**; ela não exige instalação de dependências npm:

```sh
npm --prefix services/peppy-hub test
npm --prefix services/peppy-hub run build
```

O [workflow da build nativa](.github/workflows/build-native.yml) executa os testes de downloads, enquadramento HTTP, instalação, servidor local, música, importação, autenticação, busca e navegação pelo controle, além da validação do PKG. Os testes de host e de metadados complementam a conferência no console; não a substituem.

## Página web anterior

`index.html`, `app.js`, `style.css` e `apps.json` preservam a primeira loja web estática. Ela pode ser publicada pelo GitHub Pages a partir da raiz e mantém seu próprio catálogo. Editar `apps.json` altera essa página; o botão **Adicionar URL** salva apenas no `localStorage` daquele navegador. Essa versão estática não administra contas premium nem publica o catálogo da central.

O login premium e ADM pode ser lembrado neste PS4. Ao reabrir, a conta é validada online e a biblioteca premium abre após sincronizar; sair da conta ou receber uma invalidação esquece senha/token. O FTP escolhe outra porta quando o GoldHEN já usa 2121: confira o endereço exibido no PKG, use modo passivo e login anônimo. Downloads em uma conexão agora podem sobrepor recepção e gravação com buffers limitados; a taxa continua sendo a medida da fonte e da rede.

Ao editar uma senha preenchida, o primeiro caractere digitado substitui o texto anterior. No editor, L2 limpa o campo e R3 mostra ou oculta a senha para conferir a digitação. Cancelar preserva o valor anterior. Usuários devem conter de 3 a 32 letras, números, pontos, hífens ou sublinhados; senhas devem ter de 8 a 128 bytes e diferenciam maiúsculas de minúsculas. Uma tentativa que não puder iniciar mantém a senha e mostra o motivo, sem reutilizar a mensagem de erro anterior.
