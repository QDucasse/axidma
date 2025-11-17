module axi_pattern_gen #(
    parameter DATA_WIDTH = 32,
    parameter FRAME_BYTES = 32   // frame size in bytes
)(
    input  wire                 m_axis_aclk,
    input  wire                 m_axis_aresetn,
    input  wire                 start,         // start signal from GPIO / PS
    output reg [DATA_WIDTH-1:0] m_axis_tdata,
    output reg                  m_axis_tvalid,
    output reg                  m_axis_tlast,
    output reg [3:0]            m_axis_tkeep,
    input  wire                 m_axis_tready
);

localparam BEAT_BYTES  = DATA_WIDTH/8;
localparam FRAME_BEATS = FRAME_BYTES / BEAT_BYTES;

reg [2:0] beat_cnt;                        // adjust width if FRAME_BEATS > 8
reg [DATA_WIDTH-1:0] counter;
reg running;

always @(posedge m_axis_aclk) begin
    if (!m_axis_aresetn) begin
        counter       <= 0;
        beat_cnt      <= 0;
        m_axis_tdata  <= 0;
        m_axis_tvalid <= 0;
        m_axis_tlast  <= 0;
        m_axis_tkeep  <= 0;
        running       <= 0;
    end else begin
        // Latch running when start is asserted
        if (start) running <= 1;

        if (running) begin
            m_axis_tvalid <= 1;
            m_axis_tkeep <= {DATA_WIDTH/8{1'b1}};

            if (m_axis_tready) begin
                m_axis_tdata <= counter;
                counter      <= counter + 1;

                if (beat_cnt == FRAME_BEATS-1) begin
                    m_axis_tlast <= 1;
                    beat_cnt     <= 0;
                end else begin
                    m_axis_tlast <= 0;
                    beat_cnt     <= beat_cnt + 1;
                end
            end
        end else begin
            m_axis_tvalid <= 0;
            m_axis_tlast  <= 0;
            m_axis_tkeep  <= 0;
        end
    end
end

endmodule