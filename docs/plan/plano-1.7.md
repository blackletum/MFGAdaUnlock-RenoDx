# Plano 1.7 — DLSS-G no painel, Reflex, adaptador SR→G, RTX 50; XeMFG em mais APIs

Pedidos:

- **DLSS-G:**
  1. tirar o "Force Borderless" do painel;
  2. Reflex **Off** e **On + Boost** parecem não funcionar;
  3. adaptador **DLSS-SR → DLSS-G** (gerar frames em jogos que têm só DLSS-SR);
  4. aplicar as correções de qualidade na **série RTX 50** (pedido de um usuário com 5070 Ti, log anexo).
- **XeMFG:**
  5. reduzir o **flicker da UI** com UI Composition desligada;
  6. suporte a **Vulkan**;
  7. avaliar o **pacing do driver da NVIDIA**.
- **Análise:** o projeto [`dlssg_for_sm86`](https://github.com/sdli1995/dlssg_for_sm86). Ele daria Vulkan? Quais os benefícios? Vale a pena?

Status: `pendente`, `em andamento`, `feito`, `descartado`. **[você]** marca o que precisa de você (jogo, painel, decisão).

---

## Análise

### Log do usuário RTX 5070 Ti (Witcher 3, driver 617.42)

| Achado | Consequência |
|---|---|
| O addon é a **1.4.4** (`Release 1.4.4; DLSS-G 310.9.16 …`) | O usuário está três versões atrás. Tudo o que a 1.5/1.6 trouxe falta nesse log. Pedir um log com a 1.6.0 antes de concluir qualquer coisa |
| `rewrote 2 arch gate(s) (0x1b0 -> 0x190)` numa placa **Blackwell** | A troca de arch gate existe para a Ada passar pelo caminho Blackwell. Numa RTX 50 ela é desnecessária, e talvez até nociva: o provedor passa a tratar a GB20x como AD10x nas comparações |
| `full Blackwell framework kernels applied … in original Ada slots`, `thin-geometry … applied=yes` | As correções trocam os cubins/PTX **sm_89** (`blackwell.hpp`; `thin_geometry.hpp` reescreve `.target sm_120` → `sm_89`). Uma RTX 50 executa os cubins **sm_120**, então o "applied=yes" **não muda nada na imagem** dela. O log engana: hoje as correções de qualidade não chegam à série 50 |
| Segundo provedor no `DriverStore` (`found 0 arch-gate comparisons`, `unsupported provider metadata`) | O override da NVIDIA App carrega outro `nvngx_dlssg.dll`, e o addon não o modifica (falha fechada, correto). Se o jogo usar esse provedor, nenhuma correção vale, nem na Ada |
| `numFramesToGenerate=2 (3x)` e `reports 3 frame(s) actually presented` | A geração 3x funcionou; o pedido é só de qualidade |

**Resumo para a série 50:** para a correção valer, é preciso aplicar **variantes sm_120** nos slots sm_120 do provedor, e a troca de arch gate não deve rodar quando a GPU já é Blackwell. A fonte das correções (o PTX do `BlendCandidatesFused` e do `EstimateIntermMvecsScatter`) **já é sm_120** e hoje é rebaixada para sm_89. O caminho inverso é natural, mas exige montar outra tabela de slots e conferir tudo em hardware RTX 50, que não temos.

### Reflex Off e On + Boost no DLSS-G

O código (`framecount.hpp` `SubmitReflexOptions`, `pacing_policy.hpp` `ResolveReflexMode`):

- **Off** é proposital: o Streamline continua recebendo `eLowLatency` (alguns jogos desligam o DLSS-G com o Reflex em Off) e só a espera do `slReflexSleep` é pulada. O overlay da NVIDIA e o próprio jogo continuam mostrando "Reflex On". **Hipótese 1:** "não funciona" é na verdade "não aparece como Off".
- O override só é aplicado quando o jogo chama `slReflexSetOptions`. Uma troca no painel vai para `RefreshReflexTarget`, que só reenvia **na thread dona da chamada original**. Se o jogo chama uma vez só (na abertura ou no menu), a troca fica pendente até a próxima chamada. **Hipótese 2:** a troca não chega ao driver durante o jogo.
- **On + Boost:** com o DLSS-G ligado, o `sl.dlss_g`/driver pode manter o modo próprio (como no Cyberpunk, que trava o Boost com FG). **Hipótese 3:** o Boost é aceito (`sl::Result 0`) mas ignorado.
- Não existe hoje um **efeito medido** que diferencie os modos. O painel mostra "game → effective" com base no que **enviamos**, não no que o driver aplicou.

### Adaptador DLSS-SR → DLSS-G

Já estava previsto no fechamento da 1.6 ("plano próprio"). O XeMFG já tem um adaptador DLSS-SR (`native_ngx.cpp`) que tira color, depth, MV e jitter da chamada do DLSS-SR. A ideia: usar as mesmas entradas para criar e avaliar o **DLSS-G pelo NGX/Streamline** num jogo que não o integra. Riscos: os markers do Reflex (o DLSS-G exige PCL/Reflex), a criação da swapchain/proxy de apresentação do Streamline em um jogo que não carrega `sl.interposer`, HUD-less e a câmera (matrizes) que o DLSS-SR não recebe. É o item maior do lado DLSS-G e vale um protótipo antes de se comprometer.

### Force Borderless

`dlssg_display.hpp`, chave `[DLSSG] ForceBorderless` no `MFGAdaUnlock.ini`, desligada por padrão desde a 1.5.2. Retirar do painel é simples. Falta decidir se a chave no INI continua valendo (oculta) ou se o código sai junto.

### XeMFG

- **Flicker da UI com UI Composition desligada.** Sem composição, a XeFG interpola a imagem **com a HUD já desenhada**: os textos e os elementos da HUD se deslocam com os vetores de movimento da cena e piscam nos frames gerados. Alavancas: (a) uma máscara de HUD (diferença entre o backbuffer e a cópia HUD-less, quando o jogo entrega a HUD-less) para copiar a HUD do frame real nos gerados; (b) zerar o MV/forçar a confiança nas regiões estáticas da HUD; (c) heurística sem HUD-less (pixels que não mudam entre frames reais e estão acima do conteúdo). `native_hud_copy.cpp` já faz parte disso; o item começa medindo o que ele cobre hoje.
- **Vulkan.** A XeFG e a XeLL da Intel são **só D3D12**. O [MFG-Unlocker do dashdogy](https://github.com/dashdogy/MFG-Unlocker) (MIT, a mesma base de onde o nosso XeMFG veio, commit `a4cf2c4`) resolveu isso na **v1.7.0** (25/10, commit `b97deee`) **sem ponte para o D3D12 de verdade**:
  - **`xvk`**: uma camada mínima de **D3D12 sobre Vulkan** (`source/native/xess/vulkan/xvk_*.cpp`). Ela implementa só o pedaço de D3D12/DXGI que a XeFG usa e falha fechada no resto. A `libxess_fg.dll` e a `libxell.dll` **originais da Intel** rodam sem modificação, mas no `VkDevice` do próprio jogo.
  - **Programas:** cada programa DXIL que a XeFG cria é trocado por um **SPIR-V traduzido e embutido**, casado pelo SHA-256 do DXIL (83 programas, manifesto fixo em `generate_xvk_programs.py`). Os `.spv` **não estão no repositório**; ficam embutidos no `MFGUnlocker.dll` da v1.7.0 e seguem os termos da Intel.
  - **Apresentação:** a swapchain Vulkan do jogo vira uma `VkSwapchainKHR` da camada, cujas imagens são os back buffers da XeFG. Ela precisa de Vulkan 1.3, de filas e extensões extras no `vkCreateDevice` (timeline semaphore, sync2, maintenance4, integer dot product) e opcionalmente do `VK_NV_low_latency2` (Reflex) e do `VK_AMD_anti_lag`.
  - **Entradas:** no modo standalone, só as do **DLSS-SR** (NGX Vulkan); motion vectors em resolução de tela não são suportados. Pelo fork do OptiScaler deles, qualquer entrada.
  - **Tamanho:** cerca de 430 KB de código (o `vk_host.cpp` sozinho tem 140 KB). Ele é **experimental** lá, sem lista de jogos testados além do Arknights: Endfield (pelo OptiScaler).

  **Para nós**, o caminho é **portar** (MIT, como já fizemos com o resto do XeMFG), não inventar uma ponte. As diferenças da nossa arquitetura: somos um addon do ReShade, então falta confirmar se dá para **acrescentar filas e extensões no `vkCreateDevice`** pelo ReShade (a camada Vulkan do ReShade cria o device) ou se precisamos de hook próprio no loader; e falta o lado de **entrada**: o adaptador NGX para **Vulkan** (`NVSDK_NGX_VULKAN_*`) e o DLSS-G do Streamline em Vulkan. Os SPIR-V seguiriam o mesmo esquema dos DXIL de hoje: lidos estaticamente de um binário fornecido pelo usuário, com hash conferido, fora do repositório.
- **Pacing do driver da NVIDIA.** Hoje a XeFG pacea os frames gerados pelo próprio SDK (`sdk_pacing.cpp`). Duas peças da NVIDIA são candidatas: o **Reflex pelo NVAPI** (`NvAPI_D3D_SetSleepMode/Sleep` + markers), que só controla a fila e já foi medido na 1.6 (a XeLL empata com o Reflex); e o **flip metering** em hardware das RTX 50, que só é acessível pelo próprio DLSS-G (sem API pública). Expectativa realista: **pouco ou nenhum ganho** fora da RTX 50, e na RTX 50 sem caminho público. O item é uma investigação curta com resposta "sim/não". A v1.7.0 do dashdogy trouxe uma **flip queue** nova: cada lote de frames da XeMFG é apresentado em cadência uniforme, o primeiro frame do lote assim que fica pronto, e na NVIDIA ela **funciona junto com o flip metering do driver**. Segundo as notas, isso tirou o stutter e a perda de FPS de muitos jogos. O nosso R1.8 tem um caminho próprio de flip metering (`XeFGFlipMetering`, `MeteredQueueAhead`). Antes de qualquer coisa nossa, o G compara o nosso pacing com essa flip queue. Um aviso deles também nos afeta: **com o `dxgi.dll` do ReShade a swapchain da XeFG não cresce, e em 5x/6x alguns frames gerados não chegam à tela**.

---

## Bloco 0 — Verificações antes de codar

| # | Tarefa | Resultado esperado | Est. | Status |
|---|---|---|---|---|
| 0.1 | Partir do log 1.4.4 da 5070 Ti: o código de arch gate e dos slots das correções não mudou até a 1.6.0 (não há checagem de arquitetura da GPU), então as conclusões valem. O log com a 1.6 vem com o tester do C.5, sem bloquear | conclusões do log válidas para a 1.6 | — | feito |
| 0.2 | Ler um provedor 310.9.16: existem slots sm_120 para os três kernels das correções? Com que tamanho? | viabilidade do bloco C | 2 h | pendente |
| 0.3 | Reflex: logar o modo **aplicado** (`slReflexGetState` / `NvAPI_D3D_GetSleepStatus`) depois de cada troca, e se a troca do painel ficou pendente | separa as hipóteses 1, 2 e 3 | 2 h | pendente |
| 0.4 | **[você]** Reflex: PresentMon 30 s em 1 jogo (Witcher 3 ou Cyberpunk) com Jogo / Off / On / On + Boost | diferença real de latência entre os modos | 1 h | pendente |
| 0.5 | XeFG no Vulkan: o SDK da Intel continua só D3D12; o dashdogy resolveu com uma camada D3D12-sobre-Vulkan (`xvk`, v1.7.0, MIT) | decide o bloco F (portar o `xvk`) | 30 min | feito |
| 0.6 | Medir o flicker atual (captura com UI Composition desligada, 2 jogos) e o que o `native_hud_copy` cobre | base do bloco E | 1,5 h | pendente |

---

## Bloco A — Force Borderless fora do painel (DLSS-G)

| # | Tarefa | Est. | Status |
|---|---|---|---|
| A.1 | Retirar a linha "Force Borderless" do painel (`dlssg_display.hpp`); o estado do modo de tela continua visível só como leitura | 30 min | pendente |
| A.2 | A chave `[DLSSG] ForceBorderless` e o código do hook de janela continuam; quem já tem `1` no INI segue com o comportamento atual (decisão: só sai do painel) | — | descartado (decisão) |
| A.3 | README: a chave passa a ser só do INI; testes do hook continuam valendo | 30 min | pendente |

## Bloco B — Reflex Off / On + Boost (DLSS-G)

Depende do 0.3 e do 0.4.

| # | Tarefa | Est. | Status |
|---|---|---|---|
| B.1 | Painel: mostrar o modo **aplicado pelo driver**, não só o enviado; explicar que o "Off" mantém o Reflex do Streamline ligado e só tira a espera | 1,5 h | pendente |
| B.2 | Troca no painel sem nova chamada do jogo: reenviar as opções no próximo `slReflexSleep`/Present da thread dona (o mesmo ponto seguro que já existe para o limite de FPS) | 2 h | pendente |
| B.3 | Se o Boost for ignorado com o FG ligado: marcar a opção como "sem efeito com o Frame Generation" em vez de prometer algo | 1 h | pendente |
| B.4 | Fixture: troca do painel aplicada sem nova chamada; modo aplicado lido de volta | 1,5 h | pendente |
| B.5 | **[você]** Campo: repetir o 0.4 | 1 h | pendente |

## Bloco C — Correções de qualidade na série RTX 50 (DLSS-G)

Depende do 0.2. Sem uma RTX 50 aqui, cada item precisa de um testador.

| # | Tarefa | Est. | Status |
|---|---|---|---|
| C.1 | Não reescrever arch gates quando a GPU já é Blackwell (NVAPI arch 0x1b0); o log diz que não precisa | 1 h | pendente |
| C.2 | Log honesto: "applied" só quando o slot trocado é o da arquitetura da GPU; na RTX 50, até o C.3 sair, dizer "correções de qualidade só para Ada" | 1 h | pendente |
| C.3 | Tabela de slots **sm_120** e montagem das variantes sm_120 (sem o rebaixamento para sm_89) do `BlendCandidatesFused` e do `EstimateIntermMvecsScatter`, com a mesma validação exata de tamanho e de fatbin | 6 h | pendente |
| C.4 | Opção no INI (desligada por padrão) para aplicar as correções na RTX 50, enquanto o campo não confirmar | 1 h | pendente |
| C.5 | **[você/usuário RTX 50]** Campo: mesma cena com as correções ligadas e desligadas, capturas e PresentMon | — | pendente |

## Bloco D — Adaptador DLSS-SR → DLSS-G (protótipo)

| # | Tarefa | Est. | Status |
|---|---|---|---|
| D.1 | Protótipo descartável no Streamline_Sample com o DLSS-G desligado pelo app: o addon cria e avalia o DLSS-G a partir das entradas do DLSS-SR | 6 h | pendente |
| D.2 | Decidir pelo resultado do D.1 (markers do Reflex sintéticos, proxy de apresentação, matrizes da câmera); se der, abrir um plano próprio | 1 h | pendente |

## Bloco E — Flicker da UI sem UI Composition (XeMFG)

Depende do 0.6.

| # | Tarefa | Est. | Status |
|---|---|---|---|
| E.1 | Máscara de HUD quando o jogo entrega a HUD-less: copiar os pixels da HUD do frame real sobre os gerados | 4 h | pendente |
| E.2 | Sem HUD-less: máscara de pixels estáticos (iguais nos dois frames reais, com MV zero ou desconhecido), opcional | 4 h | pendente |
| E.3 | Opção no painel (Desligada / Máscara pela HUD-less / Heurística), com a padrão decidida pela medição | 1 h | pendente |
| E.4 | **[você]** Campo: mesmos jogos do 0.6 | 1 h | pendente |

## Bloco F — XeMFG em Vulkan

Só depois do A–E. O suporte a DX11 (ponte DX11→DX12) saiu deste plano e fica para o futuro.

| # | Tarefa | Est. | Status |
|---|---|---|---|
| F.1 | Vulkan: ler o `xvk` e o `vk_host.cpp` do dashdogy (v1.7.0) e mapear o que serve para um addon do ReShade: edição do `vkCreateDevice`, troca da swapchain Vulkan, origem dos SPIR-V (recursos do `MFGUnlocker.dll` v1.7.0, lidos sem carregar o código) | 4 h | pendente |
| F.2 | Vulkan: adaptador de entrada do DLSS-SR pelo NGX Vulkan (o mesmo recorte do standalone deles) | 6 h | pendente |
| F.3 | Vulkan: portar o `xvk` e protótipo num jogo Vulkan com DLSS-SR **[você]** escolhe o jogo | 16 h | pendente |

## Bloco G — Pacing do driver da NVIDIA no XeMFG (investigação)

| # | Tarefa | Est. | Status |
|---|---|---|---|
| G.0 | Comparar o nosso flip metering (R1.8) com a flip queue da v1.7.0 do dashdogy: código e PresentMon (desvio entre frames) no mesmo jogo; ver se o aviso de 5x/6x com o `dxgi.dll` do ReShade vale para nós | 4 h | pendente |
| G.1 | Levantar o que o NVAPI expõe para pacing de apresentação (além do Reflex) e se o flip metering da RTX 50 tem acesso fora do DLSS-G | 2 h | pendente |
| G.2 | Se houver caminho: protótipo opcional, medido com PresentMon (desvio entre frames); senão, encerrar com o registro | 2 h | pendente |

## Bloco H — `dlssg_for_sm86`

| # | Tarefa | Est. | Status |
|---|---|---|---|
| H.1 | README: atualizar a seção das RTX 30 (versão 0.4.1, RTX 20 incluída, só D3D12, sem limite de borderless, motor XeSS-FG próprio) e quando usar o XeMFG ou o `dlssg_for_sm86` | 1 h | pendente |
| H.2 | *(Opcional)* **[comunidade]** Comparação A/B numa RTX 30 | — | pendente |

---

## Release

| Versão | Conteúdo | Condição |
|---|---|---|
| 1.7.0 | A, B, C.1/C.2, E (o que estiver medido), H.1 | gate completo, campo do B e do E |
| 1.7.x | C.3/C.4 (RTX 50) | campo com uma RTX 50 |
| Plano próprio | D (SR→G), F (Vulkan), G se der caminho | resultado do protótipo |

Cada release segue o processo de sempre: versão em todos os pontos (incluindo `addon_overlay.inl` e a versão numérica do recurso), notas, `fork/main` e `develop` adiantados, tag e release com o addon.

**Estimativa:** cerca de 34 h no meu lado para 0, A, B, C, E e H; D, F e G somam mais uns 39 h só de protótipo e investigação.

---

## Decisões

1. **Force Borderless:** sai só do painel; a chave do INI e o código ficam.
2. **RTX 50:** C.3 entra, validado por um testador com RTX 50 (C.5).
3. **Ordem do XeMFG:** flicker da UI (E) primeiro; Vulkan (F) depois.
5. **DX11:** fora deste plano; fica para o futuro.
4. **Branch:** `feat/1.7`, criada a partir da `fork/main` (`5c98fd8`).

### Em aberto

1. **DLSS-SR → DLSS-G:** começar pelo protótipo D.1 nesta versão ou deixar para depois do 1.7.0? Sem resposta, fica para depois do 1.7.0.

## Registro

- **2026-10-29 · análise** Leitura do log RTX 5070 Ti (addon 1.4.4), do código do Reflex (`SubmitReflexOptions`, `ResolveReflexMode`), do Force Borderless (`dlssg_display.hpp`), das correções de qualidade (`blackwell.hpp`, `thin_geometry.hpp`: slots sm_89), dos cabeçalhos do SDK da Intel (XeFG e XeLL só D3D12) e do README do `dlssg_for_sm86` v0.4.1 (só D3D12, GPLv3, RTX 20/30). Nenhum código escrito.
- **2026-10-29 · decisões** Force Borderless só sai do painel (A.2 descartado); variantes sm_120 entram com testador RTX 50; flicker da UI antes de DX11/Vulkan; branch `feat/1.7` a partir da `fork/main`.
- **2026-10-29 · Vulkan** O MFG-Unlocker do dashdogy v1.7.0 (`b97deee`, 25/10, MIT) roda a XeFG/XeLL originais no Vulkan por uma camada D3D12-sobre-Vulkan própria (`xvk`) com 83 programas SPIR-V traduzidos dos DXIL (embutidos no binário, fora do repositório). O 0.5 fecha com isso; o bloco F passa a ser portar o `xvk` (hoje F.1–F.3). A mesma versão trouxe uma flip queue nova para o pacing da XeMFG que funciona com o flip metering da NVIDIA: entra no G como G.0. Não resolve o flicker da UI: sem imagem HUD-less a HUD continua interpolada com a cena (limitação declarada deles).
- **2026-10-29 · DX11** Retirado do plano a pedido (ponte DX11→DX12, F.1/F.2 antigos); fica para o futuro. O bloco F passa a ser só Vulkan (F.1–F.3).
