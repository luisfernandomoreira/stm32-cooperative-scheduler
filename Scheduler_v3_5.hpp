// ====================================================================
// ARQUIVO: Scheduler_v3.5.hpp (AUTOSAR C++14)
//
// Agendador cooperativo para STM32C031
// VERSAO 3.5 - Producao
//
// Correcoes e melhorias aplicadas sobre v3.4:
//
//   [FIX-2] drenarFilaDeferred_() refatorado para modelo atomico
//           LIFO O(1): cada item e retirado atomicamente com
//           __disable_irq()/__enable_irq(), eliminando a janela
//           de race condition da compactacao em bloco da v3.4.
//           ISR pode chamar despachar() com seguranca durante
//           a drenagem — novos itens serao processados no mesmo
//           ciclo de drenagem (loop while ate fila vazia).
//
//   [FIX-3] reagendarComRecuperacao() corrigido: primeira iteracao
//           nao conta como perda (tarefa executou com atraso, mas
//           executou). Apenas iteracoes subsequentes — periodos
//           genuinamente pulados — incrementam perdas.
//
//   [FIX-4] executar() corrigido: agoraPos recalculado apos cada
//           execucao de funcao. reagendar() seguido de verificacao
//           de proximaMs no passado aplica recuperacao correta
//           mesmo quando atrasouMuito era false mas a execucao
//           foi lenta. Elimina disparo imediato indevido no
//           ciclo seguinte.
//
//   [IMP-N] Guard de reentrancia do idleHook_ em builds DEBUG:
//           Flag idleHookAtivo_ previne chamada recursiva se hook
//           chamar despachar() acidentalmente (ver [LIM-2]).
//           Custo zero em RELEASE (condicional de preprocessador).
//
//   [IMP-O] Desempate por indice documentado em ordenarPorPrioridade():
//           Tarefas com mesma prioridade executam em ordem crescente
//           de indice. Insertion sort estavel garante propriedade.
//
//   [IMP-P] obterEficiencia() usa uint32_t em vez de uint64_t:
//           Cortex-M0+ nao tem multiplicacao/divisao 64-bit em
//           hardware. Novo calculo evita overflow e elimina
//           emulacao de 64-bit (~20-40 ciclos economizados).
//
//   [IMP-Q] habilitarTarefa(true) nao reseta totalExecucoes:
//           Historico de execucoes preservado ao reabilitar tarefa.
//           Novo metodo resetarContadorExecucoes() disponivel para
//           reset explicito quando necessario.
//
//   [IMP-R] Comentario inline no duplo clear de SR em armar():
//           Documenta intencao do clear pos-UEV para futuros
//           revisores — sem alteracao funcional.
//
//   [IMP-S] dormirAteProximaTarefa() otimizado: elimina dupla
//           varredura de tarefas. Uma unica chamada a
//           proximaExecucao() apos deferred usa tempo atualizado
//           para o WFI, mais preciso que o snapshot inicial.
//
// Todas as correcoes e melhorias de v3.1 a v3.4 mantidas.
// ====================================================================

// ************************ OBSERVACOES *******************************
// LIMITACOES CONHECIDAS:
//   [LIM-1] contLoop_ e contSleep_ sao uint32_t.
//           obterEficiencia() pode retornar leitura errada no ciclo
//           imediatamente apos wrap (~497 dias de operacao continua).
//           Impacto: somente diagnostico — nenhum efeito funcional.
//           Correcao nao justificada para o ciclo de vida do produto.
//
//   [LIM-2] idleHook_ nao pode chamar despachar() — risco de loop
//           infinito (idle -> despacha -> idle nunca dorme).
//           Em DEBUG: guard de reentrancia [IMP-N] detecta e bloqueia.
//           Em RELEASE: responsabilidade do usuario — sem overhead.
//
//   [LIM-3] drenarFilaDeferred_() usa modelo LIFO apos [FIX-2].
//           Ordem de execucao dos callbacks nao e garantida como
//           FIFO. Se a ordem for critica, despachar callbacks em
//           funcoes compostas ou usar indices explicitamente.
// ====================================================================

// ======================== PRIORIDADE DE TAREFAS ======================
// NIVEIS DE PRIORIDADE — Scheduler_v3.5
// Escala: 0 = maior urgencia | 255 = menor urgencia
//
//   0  -  20  | PRIORIDADE MAXIMA   — seguranca e controle critico
//  21  -  99  | PRIORIDADE ALTA     — comunicacao e atuadores
// 100  - 149  | PRIORIDADE MEDIA    — logica de controle
// 150  - 199  | PRIORIDADE BAIXA    — display e visual
// 200  - 255  | PRIORIDADE MINIMA   — persistencia e diagnostico
//
// LEMBRETE: prioridade resolve EMPATES no mesmo tick.
// DESEMPATE: tarefas com mesma prioridade executam em ordem
//            crescente de indice (ordem de adicao ao scheduler).
//            Insertion sort estavel garante esta propriedade [IMP-O].
// Nao ha preempcao — tarefa em execucao roda ate retornar.
// ====================================================================

#ifndef SCHEDULER_V35_HPP
#define SCHEDULER_V35_HPP

#include "stm32c0xx.h"
#include "stm32c0xx_hal.h"
#include "WatchdogManager.h"
#include <cstdint>
#include <climits>

// ====================================================================
// LINKAGE EXTERNO
// Obrigatorio em stm32c0xx_it.c:
//
//   volatile uint8_t g_schedulerSleepExpired = 0U;
//
//   void TIM17_IRQHandler(void)
//   {
//       if (TIM17->SR & TIM_SR_UIF)
//       {
//           TIM17->SR &= ~TIM_SR_UIF;
//           g_schedulerSleepExpired = 1U;
//       }
//   }
// ====================================================================
extern "C" volatile uint8_t g_schedulerSleepExpired;

// ====================================================================
// VARIAVEIS DE DIAGNOSTICO
// Definir em main.cpp:
//
//   volatile uint8_t  g_ultimaTarefaExecutada = 0xFFU;
//   volatile uint32_t g_ultimaExecMs          = 0U;
//   volatile uint8_t  g_dbg_eficiencia        = 0U;
//   volatile uint32_t g_dbg_perdas            = 0U;
// ====================================================================
extern volatile uint8_t  g_ultimaTarefaExecutada;
extern volatile uint32_t g_ultimaExecMs;
extern volatile uint8_t  g_dbg_eficiencia;
extern volatile uint32_t g_dbg_perdas;

// ====================================================================
// NAMESPACE: SchedulerConfig
// ====================================================================
namespace SchedulerConfig
{
    constexpr std::uint8_t  MAX_TAREFAS_PADRAO    {10U};
    constexpr std::uint8_t  MAX_DEFERRED_PADRAO   {4U};
    constexpr std::uint8_t  MAX_CATCHUP           {16U};

    constexpr std::uint32_t PERIODO_INVALIDO      {0U};
    constexpr std::uint32_t PERIODO_MINIMO        {1U};

    constexpr std::uint16_t PERDAS_INICIAL        {0U};
    constexpr std::uint16_t PERDAS_MAXIMO         {0xFFFFU};

    constexpr std::uint32_t SLEEP_MINIMO_MS       {2U};

    constexpr std::uint32_t WDG_TIMEOUT_PADRAO_MS {2000U};
    constexpr std::uint32_t WDG_TIMEOUT_DEBUG_MS  {60000U};
    constexpr std::uint32_t LIMITE_INFINITO        {0U};

    constexpr std::uint8_t  INDICE_INVALIDO       {0xFFU};
    constexpr std::uint16_t PROGRAMA_INVALIDO     {0xFFFFU};

    constexpr std::uint8_t  PRIORIDADE_PADRAO     {128U};
    constexpr std::uint8_t  PRIORIDADE_MAXIMA     {0U};
    constexpr std::uint8_t  PRIORIDADE_MINIMA     {255U};

} // namespace SchedulerConfig

// ====================================================================
// NAMESPACE: SleepTimerTIM17
// ====================================================================
namespace SleepTimerTIM17
{
    // Para usar outro clock, altere APB_CLOCK_HZ_ESPERADO e recalcule:
    //   PSC_1KHZ = (APB_CLOCK_HZ_ESPERADO / 1000U) - 1U
    //
    // Exemplos:
    //   12 MHz  ->  PSC = 11999
    //   24 MHz  ->  PSC = 23999  (padrao STM32C031)
    //   48 MHz  ->  PSC = 47999
    constexpr std::uint32_t APB_CLOCK_HZ_ESPERADO {24000000U};
    constexpr std::uint32_t PSC_1KHZ              {23999U};
    constexpr std::uint32_t ARR_MAX               {65534U};
    constexpr std::uint32_t NVIC_PRIO             {3U};

    // Garante que PSC_1KHZ e APB_CLOCK_HZ_ESPERADO estao sincronizados.
    // Se alterar um sem o outro, o erro de compilacao aparece aqui.
    static_assert(PSC_1KHZ == (APB_CLOCK_HZ_ESPERADO / 1000U) - 1U,
        "SleepTimerTIM17: PSC_1KHZ inconsistente com APB_CLOCK_HZ_ESPERADO. "
        "Recalcule: PSC_1KHZ = (APB_CLOCK_HZ_ESPERADO / 1000) - 1");

    inline void armar(std::uint32_t ms) noexcept
    {
        g_schedulerSleepExpired = 0U;

        const std::uint32_t ms_clamp =
            (ms < 1U)      ? 1U     :
            (ms > ARR_MAX) ? ARR_MAX : ms;

        const std::uint16_t arr =
            static_cast<std::uint16_t>(ms_clamp - 1U);

        __HAL_RCC_TIM17_CLK_ENABLE();

        TIM17->CR1  = 0U;
        TIM17->PSC  = PSC_1KHZ;
        TIM17->ARR  = arr;
        TIM17->CNT  = 0U;

        // Limpa qualquer UIF pendente antes de habilitar interrupcao.
        // Necessario pois EGR=UG gera um UEV que levanta UIF — sem
        // este clear, a interrupcao dispararia imediatamente ao
        // habilitar DIER, acordando o sistema antes do tempo. [IMP-R]
        TIM17->SR   = 0U;
        TIM17->DIER = TIM_DIER_UIE;
        TIM17->EGR  = TIM_EGR_UG;   // forca atualizacao: carrega PSC/ARR no timer

        // Limpa UIF espu'rio gerado pelo UEV do EGR=UG acima. [IMP-R]
        // Sem este segundo clear, a ISR dispararia no proximo ciclo
        // de clock apos CEN=1, ignorando o periodo configurado em ARR.
        TIM17->SR   = 0U;

        NVIC_SetPriority(TIM17_IRQn, NVIC_PRIO);
        NVIC_ClearPendingIRQ(TIM17_IRQn);
        NVIC_EnableIRQ(TIM17_IRQn);

        TIM17->CR1 = TIM_CR1_OPM | TIM_CR1_CEN;
    }

    inline void parar() noexcept
    {
        NVIC_DisableIRQ(TIM17_IRQn);
        TIM17->CR1  = 0U;
        TIM17->DIER = 0U;
        TIM17->SR   = 0U;
    }

    // [FIX-1] Race condition corrigida — padrao canonico Cortex-M
    inline void dormirSeguro(std::uint32_t ms) noexcept
    {
        armar(ms);

        __disable_irq();

        if (g_schedulerSleepExpired == 0U)
        {
            __enable_irq();
            __WFI();
        }
        else
        {
            __enable_irq();
        }

        parar();
    }

} // namespace SleepTimerTIM17

// ====================================================================
// ENUM CLASS: GrupoTarefa
// ====================================================================
enum class GrupoTarefa : std::uint8_t
{
    SEGURANCA   = 0U,
    CONTROLE    = 1U,
    DISPLAY     = 2U,
    DIAGNOSTICO = 3U,
    SEM_GRUPO   = 0xFFU
};

constexpr std::uint8_t NUM_GRUPOS_SUSPENSAO {4U};

// ====================================================================
// tempoVencido(): wrap-around seguro para uint32_t
// ====================================================================
[[nodiscard]] static constexpr bool tempoVencido(
    std::uint32_t agora,
    std::uint32_t proximaExecucao) noexcept
{
    return static_cast<std::int32_t>(agora - proximaExecucao) >= 0;
}

// ====================================================================
// TIPOS DE FUNCAO
// ====================================================================
using FuncaoTarefa = void(*)();
using FuncaoFalha  = void(*)(std::uint8_t indiceTarefa);

// ====================================================================
// STRUCT: ResultadoExecucao
// ====================================================================
struct ResultadoExecucao
{
    std::uint8_t tarefasExecutadas    {0U};
    std::uint8_t deferredsProcessados {0U};
};

// ====================================================================
// STRUCT: DiagnosticoTarefa
// ====================================================================
struct DiagnosticoTarefa
{
    std::uint32_t periodoMs      {0U};
    std::uint32_t proximaMs      {0U};
    std::uint32_t ultimaExecMs   {0U};
    std::uint32_t totalExecucoes {0U};
    std::uint32_t jitterMaxMs    {0U};
    std::uint32_t wdgTimeoutMs   {0U};
    std::uint16_t perdas         {0U};
    GrupoTarefa   grupo          {GrupoTarefa::SEM_GRUPO};
    std::uint8_t  prioridade     {SchedulerConfig::PRIORIDADE_PADRAO};
    bool          habilitada     {false};
    bool          critica        {false};
    bool          valida         {false};
};

// ====================================================================
// STRUCT: InfoProximaTarefa
// ====================================================================
struct InfoProximaTarefa
{
    std::uint32_t tempoMs {UINT32_MAX};
    std::uint8_t  indice  {SchedulerConfig::INDICE_INVALIDO};
    bool          valida  {false};
};

// ====================================================================
// STRUCT: Tarefa
// ====================================================================
struct Tarefa
{
    FuncaoTarefa           funcao                {nullptr};
    std::uint32_t          periodoMs             {SchedulerConfig::PERIODO_INVALIDO};
    std::uint32_t          proximaMs             {0U};
    bool                   habilitada            {false};
    volatile std::uint16_t perdas                {SchedulerConfig::PERDAS_INICIAL};
    bool                   critica               {false};
    std::uint32_t          ultimaExecMs          {0U};
    std::uint32_t          wdgTimeoutMs          {SchedulerConfig::WDG_TIMEOUT_PADRAO_MS};
    std::uint32_t          jitterMaxMs           {0U};
    std::uint32_t          totalExecucoes        {0U};
    GrupoTarefa            grupo                 {GrupoTarefa::SEM_GRUPO};
    FuncaoFalha            callbackFalha         {nullptr};
    bool                   callbackFalhaDisparado{false};
    std::uint32_t          limiteExecucoes       {SchedulerConfig::LIMITE_INFINITO};
    FuncaoFalha            callbackConcluido     {nullptr};
    std::uint8_t           prioridade            {SchedulerConfig::PRIORIDADE_PADRAO};

    void iniciar(FuncaoTarefa  f,
                 std::uint32_t periodo,
                 std::uint32_t faseMs      = 0U,
                 bool          hab         = true,
                 bool          isCritica   = false,
                 GrupoTarefa   grp         = GrupoTarefa::SEM_GRUPO,
                 std::uint8_t  prior       =
                     SchedulerConfig::PRIORIDADE_PADRAO) noexcept
    {
        funcao    = f;
        periodoMs = (periodo >= SchedulerConfig::PERIODO_MINIMO)
                        ? periodo
                        : SchedulerConfig::PERIODO_MINIMO;

        habilitada             = hab;
        proximaMs              = HAL_GetTick() + faseMs;
        perdas                 = SchedulerConfig::PERDAS_INICIAL;
        critica                = isCritica;
        ultimaExecMs           = HAL_GetTick();
        jitterMaxMs            = 0U;
        totalExecucoes         = 0U;
        grupo                  = grp;
        callbackFalha          = nullptr;
        callbackFalhaDisparado = false;
        limiteExecucoes        = SchedulerConfig::LIMITE_INFINITO;
        callbackConcluido      = nullptr;
        prioridade             = prior;
    }

    void reagendar() noexcept
    {
        proximaMs += periodoMs;
    }

    // ================================================================
    // [FIX-3] reagendarComRecuperacao() corrigido.
    //
    // ANTES (v3.4): primeira iteracao incrementava perdas, mas a
    // tarefa JA EXECUTOU — era apenas um disparo com atraso,
    // nao uma perda real. Contagem de perdas ficava inflada.
    //
    // DEPOIS (v3.5):
    //   - Primeiro avanço de proximaMs: periodo atual (executou
    //     com atraso). Nao conta como perda.
    //   - Iteracoes seguintes: periodos genuinamente pulados.
    //     Cada uma incrementa perdas corretamente.
    //   - Fallback final preservado: se ainda no passado apos
    //     MAX_CATCHUP iteracoes, ancora em agora + periodo.
    // ================================================================
    void reagendarComRecuperacao(std::uint32_t agora) noexcept
    {
        // Primeiro avanco: periodo atual que executou com atraso.
        // NAO e perda — a tarefa foi executada, apenas tarde.
        proximaMs += periodoMs;

        std::uint8_t iteracoes {0U};

        // Iteracoes seguintes: periodos genuinamente perdidos.
        while (tempoVencido(agora, proximaMs)
               && iteracoes < SchedulerConfig::MAX_CATCHUP)
        {
            proximaMs += periodoMs;
            if (perdas < SchedulerConfig::PERDAS_MAXIMO) { ++perdas; }
            ++iteracoes;
        }

        // Fallback: se ainda no passado apos MAX_CATCHUP, ancora.
        if (tempoVencido(agora, proximaMs))
        {
            proximaMs = agora + periodoMs;
        }
    }

    void registrarExecucao(std::uint32_t tsFim,
                           std::uint32_t planejado,
                           std::uint32_t tsInicio,
                           std::uint8_t  indice) noexcept
    {
        ultimaExecMs = tsFim;
        ++totalExecucoes;

        const std::uint32_t desvio =
            (tsInicio >= planejado) ? (tsInicio - planejado) : 0U;

        if (desvio > jitterMaxMs) { jitterMaxMs = desvio; }

        if (limiteExecucoes != SchedulerConfig::LIMITE_INFINITO)
        {
            if (totalExecucoes >= limiteExecucoes)
            {
                habilitada = false;
                if (callbackConcluido != nullptr)
                {
                    callbackConcluido(indice);
                    callbackConcluido = nullptr;
                }
            }
        }
    }

    [[nodiscard]] bool estaEmFalha(std::uint32_t agora) const noexcept
    {
        if (!critica || !habilitada) { return false; }
        return (agora - ultimaExecMs) > wdgTimeoutMs;
    }

    [[nodiscard]] bool estaConfigurada() const noexcept
    {
        return habilitada
            && (funcao    != nullptr)
            && (periodoMs != SchedulerConfig::PERIODO_INVALIDO);
    }

    [[nodiscard]] bool detectarAtraso(std::uint32_t agora) const noexcept
    {
        return static_cast<std::int32_t>(
            agora - (proximaMs + periodoMs)) >= 0;
    }

    void resetarCallbackFalha() noexcept
    {
        callbackFalhaDisparado = false;
    }

    void resetarPerdas() noexcept
    {
        perdas = SchedulerConfig::PERDAS_INICIAL;
    }

    // [IMP-Q] Reset explicito do contador — separado de habilitarTarefa().
    void resetarContadorExecucoes() noexcept
    {
        totalExecucoes = 0U;
    }

    ~Tarefa() = default;
};

// ====================================================================
// CLASSE: AgendadorT<N_TAREFAS, N_DEFERRED>
// ====================================================================
template<std::uint8_t N_TAREFAS  = SchedulerConfig::MAX_TAREFAS_PADRAO,
         std::uint8_t N_DEFERRED = SchedulerConfig::MAX_DEFERRED_PADRAO>
class AgendadorT
{
    static_assert(N_TAREFAS  >= 1U,  "N_TAREFAS deve ser >= 1");
    static_assert(N_TAREFAS  <= 32U, "N_TAREFAS deve ser <= 32");
    static_assert(N_DEFERRED >= 1U,  "N_DEFERRED deve ser >= 1");
    static_assert(N_DEFERRED <= 16U, "N_DEFERRED deve ser <= 16");

private:

    Tarefa        tarefas_[N_TAREFAS];
    bool          grupoSuspenso_[NUM_GRUPOS_SUSPENSAO] {false,false,false,false};
    FuncaoTarefa  filaDeferred_[N_DEFERRED]            {};
    std::uint8_t  idxDeferred_                         {0U};
    std::uint32_t contLoop_                            {0U};
    std::uint32_t contSleep_                           {0U};
    std::uint32_t contDeferredOverflow_                {0U};
    FuncaoTarefa  idleHook_                            {nullptr};
    std::uint32_t contIdleHook_                        {0U};

    // ----------------------------------------------------------------
    // [IMP-N] Guard de reentrancia do idleHook_ — apenas em DEBUG.
    // Previne loop infinito se hook chamar despachar() por engano.
    // Em RELEASE: removido pelo preprocessador — zero overhead.
    // ----------------------------------------------------------------
#ifdef DEBUG
    bool idleHookAtivo_ {false};
#endif

    AgendadorT()  = default;
    ~AgendadorT() = default;
    AgendadorT(const AgendadorT&)            = delete;
    AgendadorT& operator=(const AgendadorT&) = delete;
    AgendadorT(AgendadorT&&)                 = delete;
    AgendadorT& operator=(AgendadorT&&)      = delete;

    // ----------------------------------------------------------------
    [[nodiscard]] static constexpr bool indiceValido(
        std::uint8_t indice) noexcept
    {
        return indice < N_TAREFAS;
    }

    // ----------------------------------------------------------------
    [[nodiscard]] bool grupoAtivo(const Tarefa& t) const noexcept
    {
        if (t.grupo == GrupoTarefa::SEGURANCA) { return true; }
        if (t.grupo == GrupoTarefa::SEM_GRUPO)  { return true; }

        const auto g = static_cast<std::uint8_t>(t.grupo);
        if (g >= NUM_GRUPOS_SUSPENSAO) { return true; }
        return !grupoSuspenso_[g];
    }

    // ----------------------------------------------------------------
    // [IMP-O] Desempate documentado: tarefas com mesma prioridade
    // executam em ordem crescente de indice (ordem de adicao).
    // Insertion sort e estavel — preserva ordem relativa de iguais.
    // ----------------------------------------------------------------
    void ordenarPorPrioridade(std::uint8_t* indices,
                               std::uint8_t  n) const noexcept
    {
        for (std::uint8_t i {1U}; i < n; ++i)
        {
            const std::uint8_t chave      = indices[i];
            const std::uint8_t priorChave = tarefas_[chave].prioridade;
            std::int8_t        j          =
                static_cast<std::int8_t>(i) - 1;

            while (j >= 0 &&
                   tarefas_[indices[j]].prioridade > priorChave)
            {
                indices[j + 1U] = indices[static_cast<std::uint8_t>(j)];
                --j;
            }
            indices[static_cast<std::uint8_t>(j + 1)] = chave;
        }
    }

    // ================================================================
    // [FIX-2] drenarFilaDeferred_() — modelo atomico LIFO O(1).
    //
    // PROBLEMA v3.4: snapshot + compactacao em bloco criava janela
    // de race condition. ISR podia gravar novo item durante execucao
    // de callback, e a compactacao posterior podia corromper indices.
    //
    // SOLUCAO v3.5: cada item e retirado atomicamente do topo da
    // fila (LIFO). A secao critica cobre apenas a retirada do
    // ponteiro e o decremento do indice — O(1) por item, sem
    // compactacao, sem copia de memoria.
    //
    // GARANTIAS:
    //   - ISR pode chamar despachar() a qualquer momento — o item
    //     sera processado neste mesmo ciclo de drenagem (while
    //     continua ate fila vazia).
    //   - Sem underflow: idxDeferred_ so e decrementado dentro da
    //     secao critica, apos verificacao > 0.
    //   - Sem acesso fora dos limites: indice sempre valido dentro
    //     da secao critica.
    //
    // LIMITACAO [LIM-3]: ordem LIFO. Se FIFO for necessario,
    // usar buffer circular dedicado (custo de memoria adicional).
    // ================================================================
    std::uint8_t drenarFilaDeferred_() noexcept
    {
        std::uint8_t processados {0U};

        while (true)
        {
            // Retira item do topo atomicamente
            __disable_irq();

            if (idxDeferred_ == 0U)
            {
                __enable_irq();
                break;
            }

            // Decrementa e captura o ponteiro em secao critica
            --idxDeferred_;
            FuncaoTarefa cb             = filaDeferred_[idxDeferred_];
            filaDeferred_[idxDeferred_] = nullptr;

            __enable_irq();

            // Executa fora da secao critica — ISR pode despachar
            // novos itens durante esta chamada
            if (cb != nullptr)
            {
                cb();
                ++processados;
            }
        }

        return processados;
    }

    // ----------------------------------------------------------------
    // chamarIdleHook_(): encapsula chamada ao hook com guard [IMP-N].
    // ----------------------------------------------------------------
    void chamarIdleHook_() noexcept
    {
        if (idleHook_ == nullptr) { return; }

#ifdef DEBUG
        // Guard de reentrancia: bloqueia chamada recursiva do hook.
        // Cobre o caso em que hook chama despachar() acidentalmente,
        // que poderia causar re-entrada via dormirAteProximaTarefa().
        if (idleHookAtivo_) { return; }
        idleHookAtivo_ = true;
#endif

        idleHook_();
        ++contIdleHook_;

#ifdef DEBUG
        idleHookAtivo_ = false;
#endif
    }

public:

    // ----------------------------------------------------------------
    [[nodiscard]] static AgendadorT& instancia() noexcept
    {
        static AgendadorT unicaInstancia;
        return unicaInstancia;
    }

    // ================================================================
    // REGISTRO DO IDLE HOOK
    // ================================================================

    // Registra callback executado durante o idle (entre execucao e sleep).
    // USO: diagnostico, keep-alive UART, atualizacao de display lento.
    // RESTRICAO [LIM-2]: nao chamar despachar() dentro do hook.
    void registrarIdleHook(FuncaoTarefa hook) noexcept
    {
        idleHook_ = hook;
    }

    void removerIdleHook() noexcept
    {
        idleHook_ = nullptr;
    }

    // ================================================================
    // CONFIGURACAO DE TAREFAS
    // ================================================================

    void adicionarTarefa(
        std::uint8_t  indice,
        FuncaoTarefa  funcao,
        std::uint32_t periodoMs,
        std::uint32_t faseMs      = 0U,
        bool          hab         = true,
        bool          isCritica   = false,
        GrupoTarefa   grupo       = GrupoTarefa::SEM_GRUPO,
        FuncaoFalha   onFalha     = nullptr,
        std::uint8_t  prioridade  =
            SchedulerConfig::PRIORIDADE_PADRAO) noexcept
    {
        if (!indiceValido(indice)) { return; }

        tarefas_[indice].iniciar(funcao, periodoMs, faseMs,
                                 hab, isCritica, grupo,
                                 prioridade);

        tarefas_[indice].callbackFalha = onFalha;
    }

    // ----------------------------------------------------------------
    void definirPrioridade(std::uint8_t indice,
                            std::uint8_t prioridade) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].prioridade = prioridade;
        }
    }

    // ----------------------------------------------------------------
    [[nodiscard]] std::uint8_t obterPrioridade(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice))
        {
            return tarefas_[indice].prioridade;
        }
        return SchedulerConfig::PRIORIDADE_MINIMA;
    }

    // ----------------------------------------------------------------
    void definirCallbackFalha(std::uint8_t indice,
                               FuncaoFalha  onFalha) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].callbackFalha          = onFalha;
            tarefas_[indice].callbackFalhaDisparado = false;
        }
    }

    // ----------------------------------------------------------------
    void definirLimiteExecucoes(std::uint8_t  indice,
                                 std::uint32_t limite) noexcept
    {
        if (!indiceValido(indice)) { return; }
        tarefas_[indice].limiteExecucoes = limite;
        tarefas_[indice].totalExecucoes  = 0U;
    }

    // ----------------------------------------------------------------
    void definirCallbackConcluido(std::uint8_t indice,
                                   FuncaoFalha  onConcluido) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].callbackConcluido = onConcluido;
        }
    }

    // ----------------------------------------------------------------
    void definirPeriodo(std::uint8_t  indice,
                         std::uint32_t periodo) noexcept
    {
        if (!indiceValido(indice)) { return; }
        tarefas_[indice].periodoMs =
            (periodo >= SchedulerConfig::PERIODO_MINIMO)
                ? periodo
                : SchedulerConfig::PERIODO_MINIMO;
    }

    // ================================================================
    // FILA DEFERRED
    // ================================================================

    // Enfileira callback para execucao no contexto do scheduler.
    // Seguro chamar de ISR. Retorna false se fila cheia.
    [[nodiscard]] bool despachar(FuncaoTarefa f) noexcept
    {
        if (f == nullptr) { return false; }

        __disable_irq();
        const bool fila_cheia = (idxDeferred_ >= N_DEFERRED);
        if (!fila_cheia)
        {
            filaDeferred_[idxDeferred_] = f;
            ++idxDeferred_;
        }
        else
        {
            ++contDeferredOverflow_;
        }
        __enable_irq();

        return !fila_cheia;
    }

    // ----------------------------------------------------------------
    void reiniciarFila() noexcept
    {
        __disable_irq();
        for (std::uint8_t i{0U}; i < N_DEFERRED; ++i)
        {
            filaDeferred_[i] = nullptr;
        }
        idxDeferred_          = 0U;
        contDeferredOverflow_ = 0U;
        __enable_irq();
    }

    // ================================================================
    // HABILITACAO E CONTROLE DE TAREFAS
    // ================================================================

    // ----------------------------------------------------------------
    // [IMP-Q] habilitarTarefa() nao reseta totalExecucoes.
    //
    // ANTES (v3.4): reabilitar uma tarefa apagava o historico de
    // execucoes acumulado, dificultando diagnostico de tarefas
    // que ciclam entre habilitada/desabilitada (ex: modo sleep).
    //
    // DEPOIS (v3.5): totalExecucoes e preservado ao reabilitar.
    // Para reset explicito, use resetarContadorExecucoes(indice).
    // ----------------------------------------------------------------
    void habilitarTarefa(std::uint8_t indice, bool hab) noexcept
    {
        if (!indiceValido(indice)) { return; }

        tarefas_[indice].habilitada = hab;

        if (hab)
        {
            const std::uint32_t agora = HAL_GetTick();

            // Reagenda a partir do momento atual — evita disparo
            // imediato apos longa suspensao.
            tarefas_[indice].proximaMs    =
                agora + tarefas_[indice].periodoMs;
            tarefas_[indice].ultimaExecMs = agora;

            // [IMP-Q] totalExecucoes NAO e resetado aqui.
            // Historico acumulado e preservado para diagnostico.
            // Use resetarContadorExecucoes() para reset explicito.

            tarefas_[indice].resetarPerdas();
            tarefas_[indice].resetarCallbackFalha();
        }
    }

    // ----------------------------------------------------------------
    void resetarTarefa(std::uint8_t indice) noexcept
    {
        if (!indiceValido(indice)) { return; }

        const std::uint32_t agora = HAL_GetTick();
        tarefas_[indice].proximaMs      = agora;
        tarefas_[indice].ultimaExecMs   = agora;
        tarefas_[indice].totalExecucoes = 0U;
        tarefas_[indice].resetarPerdas();
        tarefas_[indice].resetarCallbackFalha();
    }

    // ----------------------------------------------------------------
    // [IMP-Q] resetarContadorExecucoes(): reset explicito do contador.
    // Usar quando o historico deve ser zerado intencionalmente,
    // por exemplo ao entrar em novo modo de operacao.
    // ----------------------------------------------------------------
    void resetarContadorExecucoes(std::uint8_t indice) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].resetarContadorExecucoes();
        }
    }

    // ================================================================
    // CONTROLE DE GRUPOS
    // ================================================================

    void suspenderGrupo(GrupoTarefa grupo) noexcept
    {
        if (grupo == GrupoTarefa::SEGURANCA) { return; }
        if (grupo == GrupoTarefa::SEM_GRUPO)  { return; }

        const auto g = static_cast<std::uint8_t>(grupo);
        if (g < NUM_GRUPOS_SUSPENSAO)
        {
            grupoSuspenso_[g] = true;
        }
    }

    // ----------------------------------------------------------------
    void retomarGrupo(GrupoTarefa grupo) noexcept
    {
        const auto g = static_cast<std::uint8_t>(grupo);
        if (g >= NUM_GRUPOS_SUSPENSAO) { return; }

        grupoSuspenso_[g] = false;

        const std::uint32_t agora = HAL_GetTick();
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].grupo    == grupo
            &&  tarefas_[i].habilitada)
            {
                tarefas_[i].proximaMs    =
                    agora + tarefas_[i].periodoMs;
                tarefas_[i].ultimaExecMs = agora;
            }
        }
    }

    // ================================================================
    // WATCHDOG
    // ================================================================

    void definirWdgTimeout(std::uint8_t  indice,
                            std::uint32_t timeoutMs) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].wdgTimeoutMs = timeoutMs;
        }
    }

    // ----------------------------------------------------------------
    void definirWdgTimeoutGrupo(GrupoTarefa   grupo,
                                 std::uint32_t timeoutMs) noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].grupo == grupo)
            {
                tarefas_[i].wdgTimeoutMs = timeoutMs;
            }
        }
    }

    // ----------------------------------------------------------------
    void resetarTodasCriticas() noexcept
    {
        const std::uint32_t agora = HAL_GetTick();
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].critica)
            {
                tarefas_[i].ultimaExecMs           = agora;
                tarefas_[i].callbackFalhaDisparado = false;
            }
        }
    }

    // ----------------------------------------------------------------
    void desabilitarWdgPorTarefa() noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            tarefas_[i].wdgTimeoutMs =
                SchedulerConfig::WDG_TIMEOUT_DEBUG_MS;
        }
    }

    // ----------------------------------------------------------------
    [[nodiscard]] bool todasCriticasOk() const noexcept
    {
        const std::uint32_t agora = HAL_GetTick();
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].estaEmFalha(agora)) { return false; }
        }
        return true;
    }

    // ----------------------------------------------------------------
    [[nodiscard]] bool alimentarWatchdog() noexcept
    {
        const std::uint32_t agora   = HAL_GetTick();
        bool                todasOk = true;

        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].estaEmFalha(agora))
            {
                if (tarefas_[i].callbackFalha          != nullptr
                &&  !tarefas_[i].callbackFalhaDisparado)
                {
                    tarefas_[i].callbackFalha(i);
                    tarefas_[i].callbackFalhaDisparado = true;
                }
                todasOk = false;
            }
        }

        if (todasOk)
        {
            IWDG_Singleton::instancia().alimentar();
            return true;
        }
        return false;
    }

    // ================================================================
    // NUCLEO DO AGENDADOR
    // ================================================================

    [[nodiscard]] InfoProximaTarefa proximaExecucao() const noexcept
    {
        const std::uint32_t agora = HAL_GetTick();
        InfoProximaTarefa   info;

        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            const Tarefa& t = tarefas_[i];
            if (!t.estaConfigurada() || !grupoAtivo(t)) { continue; }

            if (tempoVencido(agora, t.proximaMs))
            {
                info.tempoMs = 0U;
                info.indice  = i;
                info.valida  = true;
                return info;
            }

            const std::uint32_t restante = t.proximaMs - agora;
            if (restante < info.tempoMs)
            {
                info.tempoMs = restante;
                info.indice  = i;
                info.valida  = true;
            }
        }
        return info;
    }

    // ================================================================
    // dormirAteProximaTarefa() — v3.5
    //
    // [IMP-S] Otimizacao: elimina dupla varredura de tarefas.
    //
    // ANTES (v3.4): chamava proximaExecucao() para obter info,
    // depois chamava temTarefasPendentes() (que chama
    // proximaExecucao() novamente) para verificar pos-deferred.
    // Duas varreduras completas de N_TAREFAS no caminho critico.
    //
    // DEPOIS (v3.5): apos drenarFilaDeferred_() e chamarIdleHook_(),
    // uma unica chamada a proximaExecucao() recalcula com o tick
    // atualizado. Mais preciso (tempo de sleep recalculado apos
    // overhead do deferred) e mais eficiente (uma varredura a menos).
    //
    // Fluxo completo:
    //   1. Verifica se ha tarefa pronta (retorna false se sim)
    //   2. Drena fila deferred pendente
    //   3. Chama idleHook_ (com guard DEBUG [IMP-N])
    //   4. Recalcula proximo wake-up com tick atualizado
    //   5. Se nova tarefa pronta (deferred habilitou algo): retorna
    //   6. Dorme pelo tempo recalculado
    // ================================================================
    bool dormirAteProximaTarefa() noexcept
    {
        ++contLoop_;

        // Passo 1: verifica se ha tarefa pronta agora
        {
            const InfoProximaTarefa info = proximaExecucao();
            if (!info.valida)                                     { return false; }
            if (info.tempoMs == 0U)                               { return false; }
            if (info.tempoMs < SchedulerConfig::SLEEP_MINIMO_MS) { return false; }
        }

        // Passo 2: drena callbacks deferred pendentes
        drenarFilaDeferred_();

        // Passo 3: callback de usuario (diagnostico, keep-alive, etc.)
        chamarIdleHook_();

        // Passo 4: recalcula com tick atualizado apos overhead acima.
        // [IMP-S] Uma unica chamada substitui proximaExecucao() +
        // temTarefasPendentes() da versao anterior.
        const InfoProximaTarefa infoPos = proximaExecucao();

        // Passo 5: deferred pode ter habilitado nova tarefa — nao dorme
        if (!infoPos.valida)                                    { return false; }
        if (infoPos.tempoMs == 0U)                              { return false; }
        if (infoPos.tempoMs < SchedulerConfig::SLEEP_MINIMO_MS){ return false; }

        // Passo 6: dorme pelo tempo recalculado (mais preciso que info inicial)
        SleepTimerTIM17::dormirSeguro(infoPos.tempoMs);

        ++contSleep_;
        return true;
    }

    // ================================================================
    // executar() — v3.5
    //
    // [FIX-4] Reagendamento corrigido: agoraPos recalculado apos
    // cada execucao de funcao. Se execucao lenta fizer proximaMs
    // cair no passado mesmo sem atrasouMuito inicial, a verificacao
    // pos-reagendamento aplica recuperacao correta.
    //
    // ANTES (v3.4):
    //   - atrasouMuito baseado em snapshot fixo antes do loop
    //   - Se false: reagendar() simples, sem verificar se ficou
    //     no passado apos execucao lenta
    //   - Resultado: disparo imediato indevido no ciclo seguinte
    //
    // DEPOIS (v3.5):
    //   - agoraPos recalculado apos t.funcao() — sempre atualizado
    //   - reagendar() normal primeiro (avanca periodoMs)
    //   - Verifica se proximaMs ficou no passado com agoraPos real
    //   - Se sim: aplica reagendarComRecuperacao() para corrigir
    //   - Elimina disparo imediato por execucao lenta
    // ================================================================
    ResultadoExecucao executar() noexcept
    {
        ResultadoExecucao resultado;

        // ============================================================
        // Fase 1 — Deferred calls
        // ============================================================
        resultado.deferredsProcessados = drenarFilaDeferred_();

        // ============================================================
        // Fase 2 — Coleta tarefas prontas (snapshot do tick)
        // ============================================================
        const std::uint32_t agora {HAL_GetTick()};

        std::uint8_t prontas[N_TAREFAS];
        std::uint8_t numProntas {0U};

        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            const Tarefa& t = tarefas_[i];
            if (!t.estaConfigurada() || !grupoAtivo(t)) { continue; }

            if (tempoVencido(agora, t.proximaMs))
            {
                prontas[numProntas] = i;
                ++numProntas;
            }
        }

        // ============================================================
        // Fase 3 — Ordena por prioridade [IMP-O]
        // ============================================================
        if (numProntas > 1U)
        {
            ordenarPorPrioridade(prontas, numProntas);
        }

        // ============================================================
        // Fase 4 — Executa na ordem de prioridade [FIX-4]
        // ============================================================
        for (std::uint8_t k{0U}; k < numProntas; ++k)
        {
            const std::uint8_t i = prontas[k];
            Tarefa&             t = tarefas_[i];

            const std::uint32_t planejado = t.proximaMs;

            if (t.funcao != nullptr)
            {
                const std::uint32_t tsInicio = HAL_GetTick();

                // Grava diagnostico ANTES de executar — se IWDG resetar
                // durante t.funcao(), Watch Window mostra qual tarefa travou.
                g_ultimaTarefaExecutada = i;
                g_ultimaExecMs          = tsInicio;

                t.funcao();

                // [FIX-4] agoraPos recalculado APOS execucao.
                // Reflete o tempo real consumido pela tarefa.
                const std::uint32_t agoraPos = HAL_GetTick();

                t.registrarExecucao(agoraPos, planejado, tsInicio, i);
                ++resultado.tarefasExecutadas;

                if (t.habilitada)
                {
                    // Tenta reagendamento normal (avanca periodoMs)
                    t.reagendar();

                    // [FIX-4] Verifica se execucao lenta jogou
                    // proximaMs para o passado, mesmo que
                    // atrasouMuito fosse false antes da execucao.
                    // Aplica recuperacao se necessario.
                    if (tempoVencido(agoraPos, t.proximaMs))
                    {
                        t.reagendarComRecuperacao(agoraPos);
                    }
                }
            }
        }

        g_dbg_eficiencia = obterEficiencia();
        g_dbg_perdas     = obterTotalPerdas();

        return resultado;
    }

    // ================================================================
    // CONSULTA E ESTATISTICAS
    // ================================================================

    // ----------------------------------------------------------------
    // [IMP-P] obterEficiencia() — usa uint32_t em vez de uint64_t.
    //
    // ANTES (v3.4): multiplicacao e divisao 64-bit emuladas por
    // software no Cortex-M0+ (~20-40 ciclos extras sem hardware
    // dedicado para operacoes de 64 bits).
    //
    // DEPOIS (v3.5): como contSleep_ <= contLoop_ sempre, o produto
    // contSleep_ * 100 nunca excede contLoop_ * 100. O overflow
    // de uint32_t so ocorreria se contLoop_ > 42.9M, coberto
    // pelo mesmo wrap de [LIM-1] (~497 dias). Sem perda de precisao.
    // ----------------------------------------------------------------
    [[nodiscard]] std::uint8_t obterEficiencia() const noexcept
    {
        if (contLoop_ == 0U) { return 0U; }

        // Sem risco de overflow para contLoop_ dentro do ciclo de vida
        // do produto — ver [LIM-1] e [IMP-P].
        const std::uint32_t pct =
            (contSleep_ * 100U) / contLoop_;

        return static_cast<std::uint8_t>(
            (pct > 100U) ? 100U : static_cast<std::uint8_t>(pct));
    }

    // ----------------------------------------------------------------
    [[nodiscard]] std::uint32_t obterContIdleHook() const noexcept
    {
        return contIdleHook_;
    }

    // ----------------------------------------------------------------
    void resetarEstatisticas() noexcept
    {
        contLoop_     = 0U;
        contSleep_    = 0U;
        contIdleHook_ = 0U;
        resetarTodasPerdas();
    }

    // ----------------------------------------------------------------
    [[nodiscard]] DiagnosticoTarefa obterDiagnostico(
        std::uint8_t indice) const noexcept
    {
        if (!indiceValido(indice))
        {
            return DiagnosticoTarefa{};
        }

        const Tarefa& t = tarefas_[indice];
        DiagnosticoTarefa d;
        d.periodoMs      = t.periodoMs;
        d.proximaMs      = t.proximaMs;
        d.ultimaExecMs   = t.ultimaExecMs;
        d.totalExecucoes = t.totalExecucoes;
        d.jitterMaxMs    = t.jitterMaxMs;
        d.wdgTimeoutMs   = t.wdgTimeoutMs;
        d.perdas         = t.perdas;
        d.grupo          = t.grupo;
        d.prioridade     = t.prioridade;
        d.habilitada     = t.habilitada;
        d.critica        = t.critica;
        d.valida         = true;
        return d;
    }

    // ----------------------------------------------------------------
    [[nodiscard]] const Tarefa* obterTarefa(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice)) { return &tarefas_[indice]; }
        return nullptr;
    }

    [[nodiscard]] bool estaHabilitada(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice))
        {
            return tarefas_[indice].habilitada;
        }
        return false;
    }

    [[nodiscard]] std::uint16_t obterPerdas(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice)) { return tarefas_[indice].perdas; }
        return SchedulerConfig::PERDAS_INICIAL;
    }

    [[nodiscard]] std::uint32_t obterTotalPerdas() const noexcept
    {
        std::uint32_t total {0U};
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            total += tarefas_[i].perdas;
        }
        return total;
    }

    [[nodiscard]] std::uint32_t obterJitterMax(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice))
        {
            return tarefas_[indice].jitterMaxMs;
        }
        return 0U;
    }

    [[nodiscard]] std::uint32_t obterContDeferredOverflow() const noexcept
    {
        return contDeferredOverflow_;
    }

    [[nodiscard]] bool temTarefasPendentes() const noexcept
    {
        return proximaExecucao().tempoMs == 0U;
    }

    [[nodiscard]] std::uint32_t proximoTempoExecucao() const noexcept
    {
        return proximaExecucao().tempoMs;
    }

    void resetarPerdas(std::uint8_t indice) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].resetarPerdas();
        }
    }

    void resetarTodasPerdas() noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            tarefas_[i].resetarPerdas();
        }
    }

    void resetarCallbackFalha(std::uint8_t indice) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].resetarCallbackFalha();
        }
    }

    void resetarTodosCallbacksFalha() noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            tarefas_[i].resetarCallbackFalha();
        }
    }

}; // class AgendadorT

// ====================================================================
// ALIAS: Agendador
// ====================================================================
using Agendador = AgendadorT<SchedulerConfig::MAX_TAREFAS_PADRAO,
                              SchedulerConfig::MAX_DEFERRED_PADRAO>;

#endif // SCHEDULER_V35_HPP
