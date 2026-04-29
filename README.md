# STM32 Cooperative Scheduler v3.5

**Agendador cooperativo de tarefas para Cortex-M0+ com prioridade, grupos, deferred queue e sleep inteligente**

> Desenvolvido e validado em produção sobre STM32C031K6T6 — 32 KB Flash / 6 KB RAM.

---

## Por que um scheduler cooperativo em vez de RTOS?

O STM32C031K6T6 é um microcontrolador Cortex-M0+ com **32 KB de Flash e 6 KB de RAM**. Um RTOS como FreeRTOS consome entre **3–5 KB de Flash** e exige **ao menos uma stack por tarefa** (~256–512 bytes cada), além de um TCB por tarefa (~40 bytes). Com 6 KB de RAM total e um firmware real com 8–10 "tarefas", o orçamento simplesmente não fecha.

| Recurso | FreeRTOS | Scheduler v3.3 |
|---------|----------|----------------|
| Flash (kernel) | ~3–5 KB | **~0 KB** (header-only) |
| RAM por tarefa | ~300–600 B (stack) | **~48 B** (struct `Tarefa`) |
| RAM total (8 tarefas) | ~2,5–5 KB | **~384 B** |
| Preempção | Sim | Não (cooperativo) |
| Determinismo temporal | Sim (prioridade real) | **Sim (por tick — dentro do ciclo)** |
| Heap | Necessário | **Zero** |
| Dependências externas | Kernel, port layer | **HAL_GetTick + 1 timer** |

### Modelo cooperativo: como funciona

Cada tarefa é uma **função C++ sem retorno** (`void (*)()`). O scheduler chama `executar()` no loop principal — cada tarefa roda até o final e cede o controle. Não há preempção. O desenvolvedor é responsável por manter as tarefas curtas.

```
loop principal:
  ┌─────────────────────────────────────────────────┐
  │  Agendador::instancia().executar()              │
  │                                                 │
  │  1. Drena fila deferred (callbacks de ISR)      │
  │  2. Coleta tarefas prontas no tick atual        │
  │  3. Ordena por prioridade (insertion sort)      │
  │  4. Executa em ordem de prioridade              │
  └─────────────────────────────────────────────────┘
  Agendador::instancia().dormirAteProximaTarefa()
    └─ WFI via TIM17 — CPU dorme até próxima tarefa
```

A combinação **cooperativo + WFI** entrega consumo de energia próximo a um RTOS tickless, sem nenhuma complexidade de kernel.

---

## Características Completas

### Núcleo

- **Template parametrizável**: `AgendadorT<N_TAREFAS, N_DEFERRED>` — tamanhos em compile-time, zero alocação dinâmica
- **Singleton Meyers**: `Agendador::instancia()` — zero indireção de ponteiro, inicialização ordenada
- **Header-only**: todo o scheduler em um único `.hpp`
- **AUTOSAR C++14**: sem `new`, sem `delete`, sem exceções, sem RTTI

### Escalonamento

- **Período configurável por tarefa** (1 ms a 4.294.967.295 ms)
- **Fase de início** (`faseMs`): offset inicial para distribuir tarefas no tempo e evitar colisões
- **Prioridade cooperativa** (0–255, 0 = maior urgência): resolve empates no mesmo tick via insertion sort estável
- **Recuperação de atraso** (`reagendarComRecuperacao`): catch-up limitado a `MAX_CATCHUP=16` ciclos — evita avalanche de execuções após bloqueio longo
- **Wrap-around seguro**: `tempoVencido()` usa aritmética com sinal em `uint32_t` — correto após ~49 dias de uptime

### Prioridade Cooperativa

```
Escala: 0 = maior urgência | 255 = menor urgência

   0 –  20  │ PRIORIDADE MÁXIMA  — segurança, watchdog
  21 –  99  │ PRIORIDADE ALTA    — comunicação, atuadores
 100 – 149  │ PRIORIDADE MÉDIA   — lógica de controle
 150 – 199  │ PRIORIDADE BAIXA   — display, visual
 200 – 255  │ PRIORIDADE MÍNIMA  — persistência, diagnóstico
```

> A prioridade resolve **empates no mesmo tick**. Não há preempção — a tarefa em execução roda até retornar.

### Grupos de Tarefas

Quatro grupos de suspensão mais `SEM_GRUPO`:

| Grupo | Valor | Comportamento |
|-------|-------|---------------|
| `SEGURANCA` | 0 | **Nunca suspenso** — protegido por hardware |
| `CONTROLE` | 1 | Suspensível |
| `DISPLAY` | 2 | Suspensível |
| `DIAGNOSTICO` | 3 | Suspensível |
| `SEM_GRUPO` | 0xFF | Sempre ativo |

```cpp
agendador.suspenderGrupo(GrupoTarefa::DISPLAY);   // suspende todo o grupo
agendador.retomarGrupo(GrupoTarefa::DISPLAY);      // retoma e reagenda automaticamente
```

### Fila Deferred (ISR → Task context)

Permite que ISRs (interrupções) enfileiem callbacks para execução no contexto do scheduler — seguro, sem race condition:

```cpp
// Dentro de uma ISR:
Agendador::instancia().despachar(minhaTarefaDeISR);

// O scheduler executa na próxima chamada a executar(),
// ANTES das tarefas periódicas do ciclo.
```

- Capacidade: `N_DEFERRED` callbacks (default: 4)
- Seção crítica com `__disable_irq()/__enable_irq()` — atômico
- Overflow contado em `contDeferredOverflow_` — diagnóstico sem crash
- Compactação automática após drenagem — slots reutilizados

### Sleep Inteligente via TIM17 + WFI

```cpp
// No loop principal:
while (true)
{
    agendador.executar();
    agendador.dormirAteProximaTarefa(); // CPU dorme até a próxima tarefa
}
```

O scheduler calcula o tempo até a próxima tarefa, arma o TIM17 como one-shot e executa `WFI`. A CPU acorda apenas quando há trabalho — sem polling ativo.

**Correção de race condition (FIX-1):** padrão canônico Cortex-M:
1. Arma TIM17
2. `__disable_irq()`
3. Verifica flag `g_schedulerSleepExpired`
4. Se 0: `__enable_irq()` + `__WFI()`
5. Se 1 (IRQ chegou entre armar e desabilitar): `__enable_irq()` sem dormir

Critério de sleep: `tempoMs >= SLEEP_MINIMO_MS` (2 ms). Abaixo disso, spin-wait — overhead de TIM17 não compensa.

### Watchdog por Tarefa

Cada tarefa marcada como `critica = true` possui um timeout individual de watchdog (`wdgTimeoutMs`). O scheduler verifica periodicamente via `alimentarWatchdog()`:

- Se todas as tarefas críticas executaram dentro do prazo: alimenta o IWDG hardware
- Se alguma falhou: dispara `callbackFalha` (uma única vez por evento) e **não alimenta o IWDG** — o hardware reseta o sistema

```cpp
// No loop principal:
agendador.alimentarWatchdog(); // chama IWDG::instancia().alimentar() internamente
```

### Limite de Execuções

Tarefas com vida finita:

```cpp
agendador.definirLimiteExecucoes(INDICE_TAREFA_NACK, 1U);
agendador.definirCallbackConcluido(INDICE_TAREFA_NACK, onNACKConcluido);
// A tarefa executa 1x, chama o callback e se desabilita automaticamente
```

### Diagnóstico em Tempo Real

Variáveis globais `volatile` para inspeção no debugger (Watch Window / Live Expressions):

| Variável | Tipo | Descrição |
|----------|------|-----------|
| `g_ultimaTarefaExecutada` | `uint8_t` | Índice da última tarefa executada — revela qual tarefa travou o sistema em caso de reset por IWDG |
| `g_ultimaExecMs` | `uint32_t` | Timestamp de início da última execução |
| `g_dbg_eficiencia` | `uint8_t` | % de ciclos em sleep (0–100) |
| `g_dbg_perdas` | `uint32_t` | Total de deadline misses acumuladas em todas as tarefas |

Estrutura `DiagnosticoTarefa` por índice:

```cpp
DiagnosticoTarefa d = agendador.obterDiagnostico(INDICE_TAREFA);
// d.jitterMaxMs    — pior jitter registrado (ms)
// d.totalExecucoes — total de execuções desde o reset
// d.perdas         — deadline misses desta tarefa
// d.wdgTimeoutMs   — timeout watchdog configurado
```

---

## Correções Aplicadas na v3.5

### FIX-2 — Race condition na fila deferred (`drenarFilaDeferred_()`)

**Problema (v3.4):** o modelo de snapshot + compactação em bloco deixava uma janela de race condition. Se uma ISR chamasse `despachar()` durante a execução de um callback drenado, a compactação posterior podia corromper índices.

**Correção:** modelo atômico LIFO O(1) — cada item é retirado atomicamente do topo com `__disable_irq()/__enable_irq()`. A seção crítica cobre apenas a retirada do ponteiro e o decremento do índice:

```cpp
// Retira item do topo — seção crítica mínima
__disable_irq();
--idxDeferred_;
FuncaoTarefa cb             = filaDeferred_[idxDeferred_];
filaDeferred_[idxDeferred_] = nullptr;
__enable_irq();

cb(); // executa fora da seção crítica — ISR pode despachar novos itens aqui
```

ISRs podem chamar `despachar()` com segurança durante a drenagem — novos itens são processados no mesmo ciclo (`while` continua até fila vazia). Ver [LIM-3] para a implicação de ordem LIFO.

### FIX-3 — Contagem inflada de perdas em `reagendarComRecuperacao()`

**Problema (v3.4):** a primeira iteração incrementava `perdas`, mas a tarefa **já tinha executado** — era apenas um disparo com atraso, não uma perda real.

**Correção:** primeiro avanço de `proximaMs` não conta como perda. Apenas iterações subsequentes (períodos genuinamente pulados) incrementam o contador:

```cpp
// Primeiro avanço: tarefa executou com atraso — NÃO é perda
proximaMs += periodoMs;

// Iterações seguintes: períodos realmente perdidos
while (tempoVencido(agora, proximaMs) && iteracoes < MAX_CATCHUP)
{
    proximaMs += periodoMs;
    ++perdas;   // perda real
    ++iteracoes;
}
```

### FIX-4 — Disparo imediato indevido após execução lenta (`executar()`)

**Problema (v3.4):** `atrasouMuito` era avaliado com snapshot fixo antes do loop de execução. Se a execução de uma tarefa fosse lenta, `proximaMs` caía no passado mesmo com `atrasouMuito == false` — disparando a tarefa novamente no ciclo seguinte sem recuperação.

**Correção:** `agoraPos` recalculado após `t.funcao()`. Após `reagendar()` normal, verifica se `proximaMs` ficou no passado e aplica recuperação quando necessário:

```cpp
t.funcao();
const uint32_t agoraPos = HAL_GetTick(); // timestamp real pós-execução

t.reagendar();  // avança periodoMs normalmente

if (tempoVencido(agoraPos, t.proximaMs))
{
    t.reagendarComRecuperacao(agoraPos); // corrige se execução foi lenta
}
```

---

## Melhorias Aplicadas na v3.5

### IMP-N — Guard de reentrância do `idleHook_` em builds DEBUG

Flag `idleHookAtivo_` previne chamada recursiva se o hook chamar `despachar()` acidentalmente (ver [LIM-2]). Custo zero em RELEASE — bloco removido pelo pré-processador:

```cpp
#ifdef DEBUG
    if (idleHookAtivo_) { return; }  // bloqueia re-entrada
    idleHookAtivo_ = true;
#endif
    idleHook_();
#ifdef DEBUG
    idleHookAtivo_ = false;
#endif
```

### IMP-O — Desempate por índice documentado

Tarefas com mesma prioridade executam em **ordem crescente de índice** (ordem de adição ao scheduler). O insertion sort estável garante essa propriedade sem código extra.

### IMP-P — `obterEficiencia()` usa `uint32_t` em vez de `uint64_t`

O Cortex-M0+ não tem instrução de multiplicação/divisão 64-bit em hardware — operações `uint64_t` são emuladas por software (~20–40 ciclos extras). Como `contSleep_ <= contLoop_` é sempre verdadeiro, o produto `contSleep_ * 100` nunca excede `UINT32_MAX` dentro do ciclo de vida do produto. A emulação foi eliminada sem perda de precisão.

### IMP-Q — `habilitarTarefa(true)` preserva `totalExecucoes`

**Antes (v3.4):** reabilitar uma tarefa apagava o histórico de execuções acumulado, dificultando diagnóstico de tarefas que ciclam entre habilitada/desabilitada.

**Depois (v3.5):** `totalExecucoes` é preservado ao reabilitar. Para reset explícito, use o novo método `resetarContadorExecucoes(indice)`.

### IMP-R — Comentário no duplo clear de `SR` em `armar()`

Documenta a intenção do clear pós-UEV para futuros revisores. O `EGR=UG` gera um UEV que levanta `UIF` — sem o segundo clear, a ISR dispararia imediatamente ao habilitar `DIER`, ignorando o período configurado em `ARR`. Sem alteração funcional.

### IMP-S — `dormirAteProximaTarefa()` elimina dupla varredura

**Antes (v3.4):** chamava `proximaExecucao()` + `temTarefasPendentes()` (que chamava `proximaExecucao()` novamente) — duas varreduras completas de `N_TAREFAS`.

**Depois (v3.5):** uma única chamada a `proximaExecucao()` após `drenarFilaDeferred_()` e `chamarIdleHook_()` recalcula o próximo wake-up com tick atualizado — mais preciso e uma varredura a menos por ciclo.

---

## Limitações Conhecidas

### LIM-1 — Leitura transitória de `obterEficiencia()` após wrap

`contLoop_` e `contSleep_` são `uint32_t`. Após ~497 dias de operação contínua (wrap), `obterEficiencia()` pode retornar um valor incorreto **apenas no ciclo imediatamente após o wrap**. Impacto: somente diagnóstico — zero efeito funcional. Correção não justificada para o ciclo de vida do produto.

### LIM-2 — `idleHook_` não pode chamar `despachar()`

Chamar `despachar()` dentro do idle hook cria risco de loop infinito: `idle → despacha → idle` nunca dorme. Em DEBUG o guard [IMP-N] detecta e bloqueia. Em RELEASE é responsabilidade do usuário — sem overhead.

### LIM-3 — Fila deferred com ordem LIFO

Após [FIX-2], a drenagem segue ordem **LIFO** (último a entrar, primeiro a sair). Se a ordem FIFO for crítica, despache callbacks em funções compostas ou use índices explicitamente.

---

## API de Referência

### Configuração de Tarefas

```cpp
// Adiciona ou reconfigura uma tarefa
void adicionarTarefa(
    uint8_t      indice,
    FuncaoTarefa funcao,
    uint32_t     periodoMs,
    uint32_t     faseMs     = 0U,
    bool         hab        = true,
    bool         isCritica  = false,
    GrupoTarefa  grupo      = GrupoTarefa::SEM_GRUPO,
    FuncaoFalha  onFalha    = nullptr,
    uint8_t      prioridade = SchedulerConfig::PRIORIDADE_PADRAO
);

// Ajusta período em runtime
void definirPeriodo(uint8_t indice, uint32_t periodoMs);

// Ajusta prioridade em runtime
void definirPrioridade(uint8_t indice, uint8_t prioridade);

// Habilita/desabilita tarefa
// Ao habilitar: reagenda a partir de agora, preserva totalExecucoes [IMP-Q]
void habilitarTarefa(uint8_t indice, bool hab);

// Força execução imediata na próxima chamada a executar() (zera totalExecucoes)
void resetarTarefa(uint8_t indice);

// Reset explícito do contador de execuções — use após habilitarTarefa() [IMP-Q]
void resetarContadorExecucoes(uint8_t indice);
```

### Watchdog por Tarefa

```cpp
// Define timeout individual (default: 2000 ms)
void definirWdgTimeout(uint8_t indice, uint32_t timeoutMs);

// Define timeout para todo um grupo
void definirWdgTimeoutGrupo(GrupoTarefa grupo, uint32_t timeoutMs);

// Desabilita watchdog por tarefa (modo debug — 60 s)
void desabilitarWdgPorTarefa();

// Alimenta IWDG se todas as tarefas críticas estão OK
// Retorna true se alimentou, false se alguma tarefa falhou
bool alimentarWatchdog();
```

### Grupos

```cpp
void suspenderGrupo(GrupoTarefa grupo); // SEGURANCA nunca é suspenso
void retomarGrupo(GrupoTarefa grupo);   // reagenda tarefas do grupo automaticamente
```

### Idle Hook

```cpp
// Registra callback executado entre execução e sleep (diagnóstico, keep-alive)
// RESTRIÇÃO [LIM-2]: não chamar despachar() dentro do hook
void registrarIdleHook(FuncaoTarefa hook);

void removerIdleHook();

// Número de vezes que o idle hook foi chamado
uint32_t obterContIdleHook();
```

### Fila Deferred

```cpp
// Enfileira callback para execução no contexto do scheduler (seguro de ISR)
// Ordem de drenagem: LIFO [LIM-3]
bool despachar(FuncaoTarefa f);

// Limpa toda a fila (use em reinicialização)
void reiniciarFila();

// Número de overflows desde o último reiniciarFila()
uint32_t obterContDeferredOverflow();
```

### Execução

```cpp
// Executa um ciclo completo (deferred + tarefas prontas)
ResultadoExecucao executar();

// Drena deferred, chama idle hook e dorme via TIM17 + WFI [IMP-S]
// Retorna false se não há tarefas ou tempo < SLEEP_MINIMO_MS
bool dormirAteProximaTarefa();
```

### Diagnóstico

```cpp
// Retorna struct com todos os campos de diagnóstico de uma tarefa
DiagnosticoTarefa obterDiagnostico(uint8_t indice);

// Eficiência de sleep: % de ciclos em que dormiu (0–100) [IMP-P]
uint8_t obterEficiencia();

// Deadline misses de uma tarefa específica
uint16_t obterPerdas(uint8_t indice);

// Soma de todas as perdas
uint32_t obterTotalPerdas();

// Pior jitter registrado (ms)
uint32_t obterJitterMax(uint8_t indice);

// Verifica se há tarefa pronta para executar agora
bool temTarefasPendentes();

// Tempo (ms) até a próxima tarefa
uint32_t proximoTempoExecucao();
```

---

## Design Pattern — Singleton (Meyers Singleton)

### O que é

O **Singleton** garante que uma classe tenha **exatamente uma instância** durante toda a vida do programa. A variante utilizada aqui é o **Meyers Singleton** — a forma mais simples, segura e eficiente em C++11/14:

```cpp
[[nodiscard]] static AgendadorT& instancia() noexcept
{
    static AgendadorT unicaInstancia;  // construído na 1ª chamada
    return unicaInstancia;             // retorna referência — zero cópia
}
```

A variável `static` local é inicializada **uma única vez**, na primeira chamada. O padrão C++11 garante que essa inicialização é thread-safe sem nenhum mutex manual.

---

### Por que Singleton tem menos overhead em sistemas embarcados

#### 1. Zero alocação dinâmica — sem heap

Em microcontroladores, `new`/`malloc` são proibidos ou perigosos: fragmentam a heap, têm latência imprevisível e podem falhar em runtime. O Meyers Singleton aloca o objeto no segmento **BSS/data** — determinado em tempo de link, custo zero em runtime:

```
┌──────────────────────────────────────┐
│  RAM do STM32C031 (6 KB)             │
│                                      │
│  .bss / .data  ← AgendadorT aqui    │
│  Stack          ← variáveis locais   │
│  (sem heap)     ← não existe         │
└──────────────────────────────────────┘
```

#### 2. Acesso por referência — zero indireção

Um ponteiro global exige uma leitura de memória extra para desreferenciar. A referência retornada pelo Singleton é resolvida pelo compilador em tempo de compilação — o endereço vira constante no código gerado:

```asm
; Ponteiro global (2 instruções no Cortex-M0+):
LDR  R0, [PC, #offset]   ; carrega endereço do ponteiro
LDR  R1, [R0]            ; desreferencia

; Singleton / referência (1 instrução):
LDR  R0, [PC, #offset]   ; endereço do objeto direto — sem indireção
```

#### 3. Inicialização determinística

Variáveis globais em C++ têm **ordem de inicialização indefinida** entre unidades de compilação (*Static Initialization Order Fiasco*). O Meyers Singleton inicializa o objeto **na primeira chamada**, garantindo que todas as dependências já existem no momento do uso.

#### 4. Flash menor — sem runtime de heap

Sem `new`/`delete`, o linker não inclui o código de gerenciamento de heap (`malloc`, `free`, controle de blocos livres). Em um MCU com 32 KB de Flash, isso representa centenas de bytes economizados.

#### 5. Sem overhead após a primeira chamada

O compilador (ARM Clang V6 com `-Os`) transforma o `static` local em uma flag de 1 bit no BSS. Após a inicialização, a flag nunca mais é falsa — o acesso se torna **uma única instrução de load**, equivalente a uma variável global:

```asm
; instancia() após a 1ª chamada — flag já setada, branch not taken
LDR  R0, =_ZN10AgendadorTE   ; endereço do objeto estático
BX   LR                       ; retorna referência
```

---

### Comparativo de abordagens

| Abordagem | Alocação | Indireção | Init. ordenada | Heap runtime | AUTOSAR |
|-----------|----------|-----------|----------------|--------------|---------|
| Variável global | BSS/data | Sim (ponteiro) | ❌ Indefinida | Não | ⚠️ ODR |
| `new` / heap | Heap (runtime) | Sim | ❌ Manual | Sim | ❌ Proibido |
| Passagem de parâmetro | Stack (ponteiro) | Sim | ✅ Manual | Não | ✅ |
| **Meyers Singleton** | **BSS/data** | **Não** | **✅ Garantida** | **Não** | **✅** |

---

### Regra AUTOSAR aplicada

> **AUTOSAR C++14 — A3-1-1:** *"It shall be possible to include any header file in multiple translation units without violating the One Definition Rule."*

O Meyers Singleton coloca a definição do objeto dentro de uma função `inline` no header. Cada translation unit vê a mesma definição; o linker garante uma única instância. Sem violação de ODR.

---

## Uso Mínimo

### 1. Definir variáveis de diagnóstico (`main.cpp`)

```cpp
volatile uint8_t  g_ultimaTarefaExecutada = 0xFFU;
volatile uint32_t g_ultimaExecMs          = 0U;
volatile uint8_t  g_dbg_eficiencia        = 0U;
volatile uint32_t g_dbg_perdas            = 0U;
```

### 2. Registrar handler do TIM17 (`stm32c0xx_it.c`)

```c
volatile uint8_t g_schedulerSleepExpired = 0U;

void TIM17_IRQHandler(void)
{
    if (TIM17->SR & TIM_SR_UIF)
    {
        TIM17->SR &= ~TIM_SR_UIF;
        g_schedulerSleepExpired = 1U;
    }
}
```

### 3. Configurar e iniciar (`main.cpp`)

```cpp
#include "Scheduler_v3_3.hpp"

// Índices (enums recomendado)
constexpr uint8_t IDX_SENSOR   = 0U;
constexpr uint8_t IDX_CONTROLE = 1U;
constexpr uint8_t IDX_DISPLAY  = 2U;

int main()
{
    HAL_Init();
    SystemClock_Config();

    auto& sch = Agendador::instancia();

    sch.adicionarTarefa(IDX_SENSOR,   taskSensor,   500U,  0U, true, true,
                        GrupoTarefa::SEGURANCA,  onSensorFalha, 10U);

    sch.adicionarTarefa(IDX_CONTROLE, taskControle, 100U, 10U, true, false,
                        GrupoTarefa::CONTROLE,   nullptr,       50U);

    sch.adicionarTarefa(IDX_DISPLAY,  taskDisplay,  200U, 20U, true, false,
                        GrupoTarefa::DISPLAY,    nullptr,       150U);

    sch.definirWdgTimeout(IDX_SENSOR, 1500U); // tarefa crítica: timeout 1,5 s

    while (true)
    {
        sch.executar();
        sch.alimentarWatchdog();
        sch.dormirAteProximaTarefa();
    }
}
```

---

## Memória Utilizada

Com `AgendadorT<10, 4>` (default) sobre Cortex-M0+ com ARM Clang V6 `-Os`:

| Recurso | Tamanho |
|---------|---------|
| `struct Tarefa` | ~48 bytes |
| `AgendadorT<10, 4>` (RAM) | ~490 bytes |
| Código gerado (Flash) | ~1,2–1,8 KB |
| Stack em `executar()` | ~32 bytes (array local de índices) |
| Heap | **0 bytes** |

Para reduzir ao mínimo absoluto:

```cpp
using AgendadorMini = AgendadorT<4, 2>; // 4 tarefas, 2 deferred slots
// RAM: ~200 bytes
```

---

## Configuração de Clock (TIM17)

O scheduler usa o TIM17 como timer one-shot para o sleep inteligente. O prescaler é calculado para gerar um tick de **1 ms** a partir do clock do APB.

### Clock padrão: 24 MHz (STM32C031 com HSI/2)

```cpp
// Dentro do namespace SleepTimerTIM17 — já configurado corretamente:
constexpr uint32_t APB_CLOCK_HZ_ESPERADO {24000000U};
constexpr uint32_t PSC_1KHZ              {23999U};   // 24000000 / 1000 - 1
```

Nenhuma alteração necessária se o APB clock for 24 MHz.

### Usando outro clock

Altere **as duas constantes juntas**. O `static_assert` interno garante que ficaram sincronizadas — se errar uma delas, o compilador avisa na hora:

```
error: static assertion failed: SleepTimerTIM17: PSC_1KHZ inconsistente com
APB_CLOCK_HZ_ESPERADO. Recalcule: PSC_1KHZ = (APB_CLOCK_HZ_ESPERADO / 1000) - 1
```

**Fórmula:**
```
PSC_1KHZ = (APB_CLOCK_HZ_ESPERADO / 1000) - 1
```

| Clock APB | `APB_CLOCK_HZ_ESPERADO` | `PSC_1KHZ` |
|-----------|------------------------|------------|
| 12 MHz | `12000000U` | `11999U` |
| **24 MHz** | **`24000000U`** | **`23999U`** ← padrão |
| 48 MHz | `48000000U` | `47999U` |
| 64 MHz | `64000000U` | `63999U` |

**Exemplo para 48 MHz:**

```cpp
// Em Scheduler_v3_3.hpp, namespace SleepTimerTIM17:
constexpr std::uint32_t APB_CLOCK_HZ_ESPERADO {48000000U};
constexpr std::uint32_t PSC_1KHZ              {47999U};

// O static_assert verifica automaticamente:
// 47999 == (48000000 / 1000) - 1  ✓  compila
```

**Erro intencional para demonstrar o guard:**

```cpp
// Alterar APB sem recalcular PSC — erro de compilação imediato:
constexpr std::uint32_t APB_CLOCK_HZ_ESPERADO {48000000U};
constexpr std::uint32_t PSC_1KHZ              {23999U};   // ← errado!

// static_assert falha:
// 23999 != (48000000 / 1000) - 1  →  erro em tempo de compilação
```

> **Importante:** o PSC controla a resolução de 1 ms do sleep. Com PSC errado, todos os timeouts do scheduler (período das tarefas, watchdog) ficam com escala incorreta — o `static_assert` existe exatamente para tornar esse erro impossível de passar despercebido.

---

## Requisitos

| Componente | Versão mínima |
|------------|---------------|
| Compilador | ARM Clang V6 / GCC 7+ com C++14 |
| HAL | STM32C0xx HAL (fornece `HAL_GetTick()`) |
| Timer hardware | TIM17 disponível (usado para sleep) |
| IWDG | Opcional — necessário apenas se `alimentarWatchdog()` for usado |
| RAM | ~500 bytes para `AgendadorT<10,4>` |
| Flash | ~1,5 KB |

Portável para qualquer Cortex-M com `HAL_GetTick()` e um timer de 16 bits. Adaptações necessárias: substituir `TIM17` pelo timer disponível e ajustar `PSC_1KHZ` conforme o clock do APB.

---

## Histórico de Versões

| Versão | Principais mudanças |
|--------|---------------------|
| **v3.5** | FIX-2 (deferred LIFO atômico, elimina race condition), FIX-3 (perdas não infladas na primeira recuperação), FIX-4 (disparo imediato indevido após execução lenta); IMP-N (guard reentrância idleHook DEBUG), IMP-O (desempate por índice documentado), IMP-P (eficiência uint32_t, elimina emulação 64-bit), IMP-Q (historico preservado ao reabilitar tarefa + `resetarContadorExecucoes()`), IMP-R (comentário duplo clear SR), IMP-S (dormirAteProximaTarefa elimina dupla varredura); idle hook |
| **v3.3** | BUG-A (overflow eficiência), BUG-B (timestamp reagendamento), BUG-C (underflow deferred); IMP-H (prioridade em `iniciar()`), IMP-I (HAL_GetTick 4×→3×) |
| **v3.2** | Prioridade cooperativa, insertion sort estável, `ordenarPorPrioridade()` |
| **v3.1** | BUG-2 (jitter só atraso de despacho), BUG-3 (catch-up limitado), BUG-4 (callback falha one-shot); `tempoVencido()` com aritmética signed |
| **v3.0** | Grupos de tarefas, suspensão/retomada, deferred queue, sleep TIM17+WFI |

---

## Licença

MIT — consulte o arquivo `LICENSE`.

---

*Validado em produção sobre STM32C031K6T6.*
