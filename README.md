Desafio Hardware
====================

​	O repositório contém os arquivos necessários para realizar uma atualização de firmware em um dispositivo NodeMCU ESP32 por meio de um cartão SD conectado via 1-Line SDMMC. As aplicações foram desenvolvidas utilizando a versão 4.2 da IDF, utilizando o FreeRTOS. O respositório é estruturado da seguinte maneira:

- *current*: pasta contendo os arquivos do projeto de firmware corrente (v1.0.1), reponsável por ler uma atualização de firmware binário nomeada *update.bin* na raíz do cartão SD. Todos os LEDs da placa permanecem apagados;

- *update*: pasta contendo os arquivos do projeto de firmware atualizado (v1.0.2), reponsável por verificar se a atualização foi bem sucedida e apagar a versão de firmware já instalada (*update.bin*) da raíz do cartão SD. O LED ligado ao pino 2 pisca a cada 1s após a verificação da atualização. Uma simulação de rollback pode ser induzida conectando o pino 4 ao GND durante a função de *diagnostic()*, como no exemplo *.\esp-idf\examples\system\ota\native_ota_example* da IDF.;

- *builds*: contém os arquivos binários da versão *current.bin* (v1.0.1) e a versão atualizada *update.bin*  (v1.0.2), separadamente.

  

## Instruções de uso

​	Inicialmente, grava-se a versão *current.bin* no dispositivo NodeMCU ESP32 com todos os pinos da conexão 1-Line SDMMC entre o cartão SD e o microcontrolador configurados (utilizadas ligações padrão dos exemplos). Os pinos *Card Detection* (CD) e *Write Protection* (WP) são ligados aos pinos 25 e 26 do NodeMCU ESP32 por padrão.

​	Gravar a versão *update.bin* na raíz de um cartão SD válido. Pode ser necessário configurar a velocidade máxima de leitura do cartão no firmware dependendo de sua classe ou do equipamento utilizado. Os pinos utilizados para piscar o LED (pino 2) e simular rollback (pino 4) também podem ser alterados no código.

​	Em seguida, rodar o firmware  *current.bin* e introduzir o cartão SD com a atualização a ser realizada. O app será responsável por montar o cartão SD, buscar o arquivo de atualização e aplicá-la com os mecanismos de update OTA. Caso concluída, o app é reiniciado e roda a nova versão de firmware. É então verificada sua validade e o arquivo instalado é apagado do cartão SD.

​	Os seguintes testes foram realizados com sucesso utilizando um cartão SD ligado via SPI de acordo com a disponibilidade do equipamento:

- Cartão SD introduzido após início do firmware:
  - Comportamento esperado: detecção de inserção via IRQ e montagem norma após conexão.
- Cartão SD removido antes do término da atualização:
  - Comportamento esperado: Atualização abortada e cartão desmontado do firmware, pode ser introduzido novamente para reiniciar atualização.
- Rollback simulado por meio do GPIO 4:
  - Comportamento esperado: Rollback realizado, voltado para a versão anterior do firmware.
- Cartão SD com WP habilitado:
  - Comportamento esperado: Atualização corre como esperado, mas, após sua conclusão, a versão instalada não é deletada do cartão SD.
- Firmaware não contém o arquivo *upgrade.bin*:
  - Comportamento esperado: O cartão é montado normalmente, mas o processo de atualização não é iniciado. O cartão pode ser removido e reinserido com a atualização gravada sem reiniciar o dispositivo.

**Notas:**

- Caso seja necessário recompilar as aplicações, as seguintes configurações devem ser mantidas no *idf.py menuconfig*:
  - Selecionar ao menos 4MB de memória flash;
  - Selecionar a tabela partição contendo uma versão de fábrica e duas OTAs;
  - Habilitar Bootloader config ---> Enable app rollback support (CONFIG_APP_ROLLBACK_ENABLE) <u>em ambos os apps</u>.

